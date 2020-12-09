#include <torch/csrc/jit/codegen/cuda/lower_loops.h>
#include <torch/csrc/jit/codegen/cuda/arith.h>
#include <torch/csrc/jit/codegen/cuda/ir_iostream.h>
#include <torch/csrc/jit/codegen/cuda/ir_utils.h>
#include <torch/csrc/jit/codegen/cuda/iter_visitor.h>
#include <torch/csrc/jit/codegen/cuda/kernel_expr_evaluator.h>
#include <torch/csrc/jit/codegen/cuda/lower2device.h>
#include <torch/csrc/jit/codegen/cuda/lower_utils.h>
#include <torch/csrc/jit/codegen/cuda/transform_replay.h>

#include <algorithm>
#include <deque>
#include <numeric>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

LoopNestGenerator::LoopNestGenerator(
    Fusion* fusion,
    const std::vector<Expr*>& exprs)
    : fusion_(fusion), ir_builder_(GpuLower::current()->kernel()) {
  generate(exprs);
}

// Create, place, and return the allocation for tv
kir::Expr* LoopNestGenerator::pushAlloc(TensorView* tv) {
  const auto gpu_lower = GpuLower::current();

  TORCH_INTERNAL_ASSERT(
      !(FusionGuard::getCurFusion()->hasInput(tv) ||
        FusionGuard::getCurFusion()->hasOutput(tv)),
      "Tried to allocate an input or output tensor.");

  const auto alloc_point = loop_utils::getAllocPoint(tv, for_loops_);
  const auto alloc_loop = alloc_point.first;
  const auto alloc_pos = alloc_point.second;

  // Grab the dimensions the allocation will be based on to compute a size
  std::vector<Val*> alloc_dims;
  for (size_t i = 0; i < tv->nDims(); i++) {
    IterDomain* compute_at_dim = tv->getComputeAtAxis(i).first;
    IterDomain* local_dim = tv->axis(i);
    const auto memory_type = tv->getMemoryType();
    if (
        // If we're reducing this dimension, don't use it in the allocation
        // computation
        local_dim->isReduction() ||
        // If this is a broadcast dimension, don't use it in the allocation
        // computation
        local_dim->isBroadcast()) {
      continue;
    }

    if ((int)i < alloc_pos) {
      // Even when the axis is outside the allocation position, if the
      // tensor is shared with respect to the axis, the buffer size
      // needs to be expanded for the axis. Sharing occurs in two
      // cases: 1) the tensor is on shared memory with the axis
      // parallelized by TIDs, and 2) the tensor is on global memory
      // with the axis parallelized by TIDs or BIDs.
      if (!((memory_type == MemoryType::Shared &&
             compute_at_dim->isThreadDim()) ||
            (memory_type == MemoryType::Global &&
             compute_at_dim->isThread()))) {
        continue;
      }
    } else {
      if (
          // If shared memory, don't use any IDs bound to a grid dimension
          (memory_type == MemoryType::Shared && compute_at_dim->isBlockDim()) ||
          // If local memory, don't use any IDs bound to a grid or block
          // dimension
          (memory_type == MemoryType::Local && compute_at_dim->isThread())) {
        continue;
      }
    }
    alloc_dims.push_back(compute_at_dim->rawExtent());
  }

  // Multiply all the dimensions we're going to use for the allocation together
  // to get the total size
  kir::Val* size = nullptr;
  if (alloc_dims.size() == 0) {
    size = ir_builder_.create<kir::Int>(1);
  } else {
    size = gpu_lower->lowerValue(alloc_dims[0]);
    for (size_t i = 1; i < alloc_dims.size(); i++) {
      size = ir_builder_.mulExpr(size, gpu_lower->lowerValue(alloc_dims[i]));
    }
  }

  // Create the allocation node
  const auto lowered_tv = ir_builder_.create<kir::TensorView>(tv);
  const auto alloc = ir_builder_.create<kir::Allocate>(
      lowered_tv, lowered_tv->memoryType(), size);

  // Track Dynamic Shared Memory Allocation Nodes
  if (tv->getMemoryType() == MemoryType::Shared) {
    if (!kir::ExpressionEvaluator::isConst(size)) {
      dynamic_smem_.push_front(alloc);
      return nullptr;
    }
  }

  // Place the allocation
  if (alloc_loop != nullptr) {
    alloc_loop->body().insert(for_loop_allocations_[alloc_loop], alloc);
    ++for_loop_allocations_[alloc_loop];
  } else {
    lowered_exprs_.insert(
        lowered_exprs_.begin() + lowered_exprs_allocations_, alloc);
    ++lowered_exprs_allocations_;
  }

  return alloc;
}

namespace {

// TODO(kir): revisit and try to simplify this
kir::ForLoop* openForHelper(kir::ForLoop* scope, IterDomain* id) {
  const auto gpu_lower = GpuLower::current();
  kir::IrBuilder ir_builder(gpu_lower->kernel());
  const auto kir_id = gpu_lower->lowerValue(id)->as<kir::IterDomain>();
  kir::ForLoop* new_scope = nullptr;
  if (id->isThread()) {
    std::stringstream ss;
    ss << id->getParallelType();
    new_scope = ir_builder.create<kir::ForLoop>(
        ir_builder.create<kir::NamedScalar>(ss.str(), DataType::Int),
        kir_id,
        scope);
  } else {
    new_scope = ir_builder.create<kir::ForLoop>(
        ir_builder.create<kir::Int>(c10::nullopt), kir_id, scope);
  }
  if (scope != nullptr) {
    scope->body().push_back(new_scope);
  }
  return new_scope;
}

} // namespace

void LoopNestGenerator::openFor(IterDomain* iter_domain) {
  if (for_loops_.size() > 0) {
    const auto new_scope = openForHelper(for_loops_.back(), iter_domain);
    for_loop_allocations_.insert({new_scope, 0});
    for_loops_.push_back(new_scope);
  } else {
    for_loops_.push_back(openForHelper(nullptr, iter_domain));
    lowered_exprs_.push_back(for_loops_.back());
  }
}

void LoopNestGenerator::closeFor() {
  TORCH_INTERNAL_ASSERT(!for_loops_.empty());
  for_loops_.pop_back();
}

void LoopNestGenerator::pushBack(kir::Expr* expr) {
  if (for_loops_.size() == 0) {
    lowered_exprs_.push_back(expr);
  } else {
    for_loops_.back()->body().push_back(expr);
  }
}

// Update for loop structure based on this TensorView, if there's an allocation
// stmt, send it in so we can make sure that we insert this initialization after
// it
void LoopNestGenerator::initReduction(
    TensorView* tv,
    Val* init_val,
    kir::Expr* alloc_expr) {
  const auto gpu_lower = GpuLower::current();

  // This is a workaround to handle size-1 reduction, i.e. squeeze ops,
  // and will be removed once we structurally refactor the way we handle
  // such reductions, i.e. convert them to SET etc.
  if (!tv->hasReduction()) {
    // Create the initialization assignment
    const auto kir_tv = gpu_lower->lowerValue(tv);
    const auto init_stmt = ir_builder_.create<kir::UnaryOp>(
        UnaryOpType::Set, kir_tv, gpu_lower->lowerValue(init_val));
    pushBack(init_stmt);
    return;
  }

  const auto alloc_point = loop_utils::getAllocPoint(tv, for_loops_);
  const auto alloc_loop = alloc_point.first;
  const auto alloc_pos = alloc_point.second;

  // Grab the IDs that will be involved in the initialization, ignore local
  // reduction dimensions. Everything else will be iterated over to cover the
  // entire buffer. Index compute will ignore [block, grid]Dims depending on
  // buffer memory location
  std::vector<kir::IterDomain*> ids;
  for (size_t i = alloc_pos; i < tv->nDims(); i++) {
    IterDomain* ca_dim = tv->getComputeAtAxis(i).first;
    IterDomain* local_dim = tv->axis(i);
    if (local_dim->isReduction())
      continue;
    ids.push_back(gpu_lower->lowerValue(ca_dim)->as<kir::IterDomain>());
  }

  // Init a pointer that will become the entirety of the initialization
  kir::Expr* init_loop_nest = nullptr;

  // The for loop that we will place the initialization within (alloc_pos - 1),
  // if one exists. Once we're done this inner_fl will be the inner most loop
  // containing the init_stmt
  kir::ForLoop* inner_fl = nullptr;
  if (alloc_pos >= 1) {
    inner_fl = for_loops_[alloc_pos - 1];
  }

  // Work through the iter domains that we need to initialize on, outside to
  // inside, to construct the loop nest for the initialization.
  for (auto id : ids) {
    kir::ForLoop* new_fl = nullptr;

    if (id->isThread()) {
      // If based on a thread, make sure we get the named Int right
      std::stringstream ss;
      ss << id->parallelType();
      new_fl = ir_builder_.create<kir::ForLoop>(
          ir_builder_.create<kir::NamedScalar>(ss.str(), DataType::Int),
          id,
          inner_fl);
    } else {
      // Otherwise it's just a new int-
      new_fl = ir_builder_.create<kir::ForLoop>(
          ir_builder_.create<kir::Int>(c10::nullopt), id, inner_fl);
    }
    for_loop_allocations_.insert({new_fl, 0});

    if (init_loop_nest == nullptr) {
      // If this is our first generated loop, then it will be our outer most
      // loop nest
      init_loop_nest = new_fl;
    } else {
      // Otherwise place it inside the last generated loop
      inner_fl->body().push_back(new_fl);
    }

    // Increment the inner most for loop
    inner_fl = new_fl;
  }

  // Create the initialization assignment
  const auto kir_tv = gpu_lower->lowerValue(tv);
  const auto init_stmt = ir_builder_.create<kir::UnaryOp>(
      UnaryOpType::Set, kir_tv, gpu_lower->lowerValue(init_val));

  // If there were for loops generated, place the init_stmt in the inner most
  // for loop. If no loops were generated, than our init_stmt is all we need.
  if (init_loop_nest == nullptr) {
    init_loop_nest = init_stmt;
  } else {
    inner_fl->body().push_back(init_stmt);
  }

  // If we don't have an alloc_loop defined it means it needs to go in
  // lowered_exprs_. Make sure to place after the allocation of what we're
  // initializing if there is one.
  if (alloc_loop == nullptr) {
    if (alloc_expr != nullptr) {
      auto it =
          std::find(lowered_exprs_.begin(), lowered_exprs_.end(), alloc_expr);
      TORCH_INTERNAL_ASSERT(
          it != lowered_exprs_.end(),
          "Could not figure out where to initialize the buffer for ",
          tv);
      lowered_exprs_.insert(it + 1, init_loop_nest);
    } else {
      lowered_exprs_.insert(lowered_exprs_.begin(), init_loop_nest);
    }
  } else {
    if (alloc_expr != nullptr) {
      // If there is an allocation for this TensorView
      // place this loop nest after it
      alloc_loop->body().insert_after(alloc_expr, init_loop_nest);
      ++for_loop_allocations_[alloc_loop];
    } else {
      // Otherwise we're allocating a global value
      alloc_loop->body().insert(0, init_loop_nest);
    }
  }
}

void LoopNestGenerator::handle(const Expr* expr) {
  const auto gpu_lower = GpuLower::current();

  // Check if it's a tensor view expression we need to place in the loop nest
  // structure
  if (!ir_utils::isTVOp(expr)) {
    for (auto out : expr->outputs()) {
      TORCH_INTERNAL_ASSERT(
          out->getValType().value() == ValType::Scalar,
          "Unrecognized output type found in expr ",
          expr,
          " cannot lower ",
          out->getValType().value());

      pushBack(ir_builder_.create<kir::Allocate>(
          gpu_lower->lowerValue(out),
          MemoryType::Local,
          ir_builder_.create<kir::Int>(1)));
    }
    pushBack(gpu_lower->lowerExpr(expr));
    return;
  }

  //  0) Apply SyncThreads if any shared memory inputs are modified
  bool shared_memory_sync = false;
  for (auto in : expr->inputs()) {
    shared_memory_sync |= isModifiedSharedMemory(in);
  }
  if (shared_memory_sync) {
    // Push "sync" to the back of the last for loop
    if (!for_loops_.empty()) {
      for_loops_.back()->body().push_back(ir_builder_.create<kir::Sync>());
    } else {
      lowered_exprs_.push_back(ir_builder_.create<kir::Sync>());
    }
    cleanSharedMemory();
  }

  TensorView* out = expr->output(0)->as<TensorView>();

  // Figure out what the entire loop structure should look like.
  std::deque<IterDomain*> loop_structure;

  // As we go through iteration domains track the previous view
  const TensorView* last_ca_view = nullptr;
  // Check where in the previous view our last axis was in that view
  int64_t last_ca_view_ind = 0;

  // Look at each axis individually in out's domain
  for (int64_t out_i = 0; out_i < (int64_t)out->getThisComputeAtAxis();
       out_i++) {
    // Grab the axis information
    auto ca_point = out->getComputeAtAxis(out_i);
    auto ca_view = ca_point.second;
    auto ca_id = ca_point.first;

    // Figure out if there are axes in the compute at tensor view that aren't
    // in out, make sure to also open them. Check where to start looking for
    // them in the compute at view.
    size_t start = 0;
    if (last_ca_view == nullptr) {
      // Start at the begining, we haven't processed any axes yet.
      start = 0;
    } else if (last_ca_view == ca_view) {
      // This view is the same as the last axis, so start where we left off.
      start = last_ca_view_ind + 1;
    } else {
      // This is a new view, figure out where we are in it, and start from there
      for (start = 0; start < ca_view->nDims(); start++) {
        if (loop_structure.back() == ca_view->getComputeAtAxis(start).first) {
          break;
        }
      }
      start++;
    }

    // Go from start, and open all loops in the computeAt view until we hit the
    // one associated with out->getComputeAtAxis(out_i)
    for (size_t ca_i = start; ca_i < ca_view->nDims(); ca_i++) {
      // Note that ca_view->getComputeAtAxis(ca_i) is equivalent to
      // std::pair(ca_view->axis(ca_i), ca_view)
      loop_structure.push_back(ca_view->getComputeAtAxis(ca_i).first);

      // Update the last view processed
      last_ca_view_ind = ca_i;
      last_ca_view = ca_view;
      if (ca_view->getComputeAtAxis(ca_i).first == ca_id) {
        break;
      }
    }

    // Shouldn't ever hit this, but make sure we hit the break above, meaning we
    // added all necessary axes from the compute at view.
    TORCH_INTERNAL_ASSERT(
        ca_view->getComputeAtAxis(last_ca_view_ind).first == ca_id);
  }

  // We're up to the compute at point in loop_structure, grab the remaining
  // axes.
  for (int64_t out_i = (int64_t)out->getThisComputeAtAxis();
       out_i < (int64_t)out->nDims();
       out_i++) {
    // It's actually local, but getComputeAtAxis returns a std::pair, axis
    // doesn't
    loop_structure.push_back(out->getComputeAtAxis(out_i).first);
  }

  // At this point loop_structure contains our overal target loop nest structure
  // Lets get a copy of the loop structure, and figure out which loops we need
  // to open.
  auto loops_to_open = loop_structure;

  // Pop out loops already opened
  for (const auto& existing_loop : for_loops_) {
    if (loops_to_open.empty()) {
      // Nothing to open
      break;
    }
    if (gpu_lower->lowerValue(loops_to_open.front())->as<kir::IterDomain>() ==
        existing_loop->iter_domain()) {
      loops_to_open.pop_front();
    }
  }

  // At this point for_loops_ + loops_to_open contains our overal target loop
  // nest structure. Open loops in "loops_to_open".
  while (!loops_to_open.empty()) {
    openFor(loops_to_open.front());
    loops_to_open.pop_front();
  }

  kir::Expr* alloc_expr = nullptr;

  // Place the allocation for out
  if (!fusion_->hasInput(out) && !fusion_->hasOutput(out)) {
    alloc_expr = pushAlloc(out);
  }

  //  If this is a reduction, initialize the output (open for loops to inner
  //  most, predicate, initialize, place next after allocation if exists, close
  //  to computeAt)
  if (out->hasReduction()) {
    initReduction(out, expr->as<ReductionOp>()->init(), alloc_expr);
  }

  //  Place the expression
  pushBack(gpu_lower->lowerExpr(expr));

  // If output is a shared memory buffer, set modified status
  modifySharedMemory(out);

  // Reduce the loop nest structure back to computeAt
  if (out->getThisComputeAtAxis() == 0) {
    while (!for_loops_.empty()) {
      closeFor();
    }
  } else {
    const auto ca_axis = out->getThisComputeAtAxis() - 1;
    const auto target_domain =
        gpu_lower->lowerValue(out->getComputeAtAxis(ca_axis).first)
            ->as<kir::IterDomain>();
    while (!for_loops_.empty() &&
           for_loops_.back()->iter_domain() != target_domain) {
      closeFor();
    }
  }
}

namespace {

TensorView* findOutputTensor(Expr* expr) {
  TORCH_INTERNAL_ASSERT(
      expr->outputs().size() <= 1, "Unexpected number of outputs");
  if (expr->outputs().size() != 1) {
    return nullptr;
  }
  auto out = expr->output(0);
  if (out->getValType() != ValType::TensorView) {
    return nullptr;
  }
  return out->as<TensorView>();
}

void findTargetTensor(Expr* expr, TensorView*& target, unsigned& score) {
  TORCH_INTERNAL_ASSERT(expr->outputs().size() <= 1);

  TensorView* out_tv = findOutputTensor(expr);
  if (out_tv == nullptr) {
    target = nullptr;
    score = 0;
    return;
  }

  if (!out_tv->hasComputeAt()) {
    target = out_tv;
    // No computeAt, so this should come last.
    score = std::numeric_limits<unsigned>::max();
    return;
  }

  // Note this returns the computeAt position
  int pos = (int)out_tv->getRelativeComputeAtAxis();
  target = out_tv->getComputeAtView();
  while (target->hasComputeAt()) {
    if ((int)target->getThisComputeAtAxis() < pos) {
      break;
    }
    // getComputeAtRelPos accepts an axis index.
    pos = pos == 0 ? 0 : target->getComputeAtRelPos(pos - 1) + 1;
    target = target->getComputeAtView();
  }

  score = pos;
}

// Type definitions for brevity
using ExprListT = std::vector<Expr*>;
using TargetGroupMapT = std::unordered_map<TensorView*, ExprListT>;
using ExprTargetMapT = std::unordered_map<Expr*, TensorView*>;
using ScoreT = unsigned;
using ExprScoreMapT = std::unordered_map<const Expr*, ScoreT>;

void sanityCheck(
    const ExprListT& exprs,
    const ExprListT& reordered_exprs,
    const ExprScoreMapT& scores,
    const ExprTargetMapT& target_map,
    const TargetGroupMapT& computed_at_exprs) {
  const auto num_exprs = exprs.size();
  TORCH_INTERNAL_ASSERT(scores.size() == num_exprs);
  TORCH_INTERNAL_ASSERT(
      reordered_exprs.size() + target_map.size() == num_exprs);
  int num_computed_exprs = std::accumulate(
      computed_at_exprs.begin(),
      computed_at_exprs.end(),
      0,
      [](int acc, const std::pair<TensorView*, ExprListT>& p) {
        return acc + p.second.size();
      });
  TORCH_INTERNAL_ASSERT(num_computed_exprs == (int)target_map.size());
}

// Arrange exprs into loop-nest groups. Loop-nest groups are
// disjoint grouping of expressions based on the expression
// where each expression is computed at.
void groupExpressions(
    Expr* expr,
    ExprListT& reordered_exprs,
    ExprTargetMapT& target_map,
    TargetGroupMapT& computed_at_exprs,
    ExprScoreMapT& scores) {
  TensorView* target_tensor = nullptr;
  ScoreT score = 0;
  findTargetTensor(expr, target_tensor, score);
  scores.emplace(expr, score);
  if (target_tensor == nullptr) {
    reordered_exprs.push_back(expr);
  } else {
    target_map.emplace(expr, target_tensor);
    if (computed_at_exprs.find(target_tensor) == computed_at_exprs.end()) {
      computed_at_exprs.emplace(target_tensor, TargetGroupMapT::mapped_type());
    }
    auto& exprs = computed_at_exprs[target_tensor];
    exprs.push_back(expr);
  }
}

// Sort each loop-nest group based on axis (i.e., score)
void sortGroup(ExprListT& exprs, ExprScoreMapT& scores) {
  std::stable_sort(
      exprs.begin(),
      exprs.end(),
      [&scores](const Expr* expr1, const Expr* expr2) {
        return scores[expr1] < scores[expr2];
      });
}

// If an expression is missing from expr_status, search for all ancestors
// that are necessary for the expression
void mapMissingInputsToAncestors(
    const TensorView* tv,
    const std::unordered_map<const Expr*, bool>& expr_status,
    std::vector<const TensorView*>& ancestors) {
  const Expr* expr = tv->definition();
  const auto& expr_inputs = ir_utils::filterByType<TensorView>(expr->inputs());
  for (auto input : expr_inputs) {
    const Expr* input_definition = input->definition();
    if (input_definition != nullptr) {
      if (expr_status.find(input_definition) == expr_status.end()) {
        mapMissingInputsToAncestors(input, expr_status, ancestors);
      } else {
        ancestors.push_back(input);
      }
    }
  }
}

// For each expression, find all TensorView inputs.
// If an input TensorView is missing from expr_status,
// find that input's ancestors that are present in expr_status.
std::unordered_map<const Expr*, std::vector<const TensorView*>> findExprTvInputs(
    const std::unordered_map<const Expr*, bool>& expr_status) {
  std::unordered_map<const Expr*, std::vector<const TensorView*>>
      map_expr_to_tv_inputs;

  // Iterate over all exprs and filter missing expr
  for (auto item : expr_status) {
    const auto expr = item.first;
    const auto& expr_inputs =
        ir_utils::filterByType<TensorView>(expr->inputs());

    map_expr_to_tv_inputs.insert({expr, std::vector<const TensorView*>()});
    auto& tv_inputs = map_expr_to_tv_inputs[expr];

    for (auto input : expr_inputs) {
      const Expr* input_definition = input->definition();
      bool missing_input = input_definition != nullptr &&
          expr_status.find(input_definition) == expr_status.end();

      if (missing_input) {
        // Map missing input to ancestor that is present in exprs_status
        std::vector<const TensorView*> ancestors;
        mapMissingInputsToAncestors(input, expr_status, ancestors);
        tv_inputs.insert(tv_inputs.begin(), ancestors.begin(), ancestors.end());
      } else {
        tv_inputs.push_back(input);
      }
    }
  }
  return map_expr_to_tv_inputs;
}

// Reorder expressions that are computed at the same position in a
// breadth-first order.
void reorderSegmentBreadthFirst(
    ExprListT::iterator seg_begin,
    ExprListT::const_iterator seg_end) {
  // mapping of each expression to a bool flag indicating if it's
  // already been visited
  std::unordered_map<const Expr*, bool> expr_status;
  for (auto it = seg_begin; it != seg_end; ++it) {
    expr_status.insert({*it, false});
  }

  // Holds all input TVs necessary for every expression.
  const auto map_expr_to_tv_inputs = findExprTvInputs(expr_status);

  while (seg_begin != seg_end) {
    std::vector<const Expr*> visited_exprs;
    for (auto it = seg_begin; it != seg_end; ++it) {
      const auto expr = *it;
      const auto& expr_inputs = map_expr_to_tv_inputs.at(expr);

      // if all input expressions are visited
      // then expr can be visited
      const bool ready_to_visit = std::all_of(
          expr_inputs.begin(),
          expr_inputs.end(),
          [&expr_status](const TensorView* input) {
            const Expr* input_definition = input->definition();
            return input_definition == nullptr ||
                (expr_status.find(input_definition) != expr_status.end() &&
                 expr_status.at(input_definition));
          });
      if (ready_to_visit) {
        std::iter_swap(seg_begin, it);
        TORCH_INTERNAL_ASSERT(*seg_begin == expr);
        ++seg_begin;
        visited_exprs.push_back(expr);
      }
    }
    for (const auto& visited_expr : visited_exprs) {
      expr_status.at(visited_expr) = true;
    }
  }
}

// Reorder expressions in a group in a breadth-first order. Reordering
// is done within a subset of expressions that have the same score
// (i.e., computeAt position). For each subset,
// reorderSegmentBreadthFirst is called.
void reorderGroupBreadthFirst(ExprListT& exprs, const ExprScoreMapT& scores) {
  auto seg_begin = exprs.begin();
  auto seg_end = exprs.begin();
  ScoreT seg_score = scores.at(*seg_begin);
  while (seg_end != exprs.end()) {
    const auto expr = *seg_end;
    const auto cur_score = scores.at(expr);
    if (seg_score == cur_score) {
      // advance further
      ++seg_end;
      continue;
    } else if (seg_score < cur_score) {
      // segment ended
      reorderSegmentBreadthFirst(seg_begin, seg_end);
      seg_begin = seg_end;
      seg_score = cur_score;
    } else {
      // exprs list is assumed to be sorted in the order of scores, so
      // this should never be reachable
      TORCH_INTERNAL_ASSERT(
          false, "Unexpected expression: ", expr, ", score: ", cur_score);
    }
  }
  reorderSegmentBreadthFirst(seg_begin, seg_end);
}

void mergeNonRootGroupsIntoRootGroups(
    TargetGroupMapT& computed_at_exprs,
    ExprTargetMapT& target_map) {
  for (auto it = computed_at_exprs.begin(); it != computed_at_exprs.end();) {
    TensorView* target = it->first;
    if (target->hasComputeAt()) {
      Expr* target_expr = target->definition();
      TensorView* target_of_target = target_map.at(target_expr);
      auto& target_group = computed_at_exprs.at(target_of_target);
      auto pos =
          std::find(target_group.begin(), target_group.end(), target_expr);
      TORCH_INTERNAL_ASSERT(pos != target_group.end());
      target_group.insert(pos, it->second.begin(), it->second.end());
      // Update the target map
      for (auto& inserted_expr : it->second) {
        TORCH_INTERNAL_ASSERT(target_map.at(inserted_expr) == target);
        target_map.at(inserted_expr) = target_of_target;
      }
      it = computed_at_exprs.erase(it);
    } else {
      ++it;
    }
  }
}

// Merge root loop-nests into reordered_exprs
void mergeGroupsIntoSortedList(
    TargetGroupMapT& computed_at_exprs,
    ExprListT& reordered_exprs) {
  while (computed_at_exprs.size() > 0) {
    // Find the root loop-nest that has no dependency with the other
    // loop-nests
    TensorView* cur_target = computed_at_exprs.begin()->first;
    for (auto& group : computed_at_exprs) {
      auto target = group.first;
      if (cur_target == target)
        continue;
      if (DependencyCheck::isDependencyOf(target, cur_target)) {
        cur_target = target;
      }
    }
    // cur_target can be visited
    reordered_exprs.insert(
        reordered_exprs.end(),
        computed_at_exprs.at(cur_target).begin(),
        computed_at_exprs.at(cur_target).end());
    computed_at_exprs.erase(cur_target);
  }
}

// Reorder exprs so that LoopNestGenerator::handle(Expr*) can generate
// correct loop nests. Vector exprs is assumed to be topologically
// sorted, but that is not sufficient as tensors computed at
// outer loops need to be located earlier.
std::vector<Expr*> reorderExprsForComputeAt(const std::vector<Expr*>& exprs) {
  ExprListT reordered_exprs;

  // expr -> target
  ExprTargetMapT target_map;

  // target -> [computed at expressions]
  TargetGroupMapT computed_at_exprs;

  // score of each expression that is calculated based on the
  // computeAt axis. A lower score of an expression means it should be
  // placed earlier in the expression list. This is a requirement for
  // the loop-nest generation of this class to work.
  ExprScoreMapT scores;

  // 1. Group expressions by target tensors. Non-grouped expressions
  // are copied into reordered_exprs.
  for (auto& expr : exprs) {
    groupExpressions(
        expr, reordered_exprs, target_map, computed_at_exprs, scores);
  }

  sanityCheck(exprs, reordered_exprs, scores, target_map, computed_at_exprs);

  // If no computeAt found, no need to reorder.
  if (computed_at_exprs.size() == 0) {
    return exprs;
  }

  // 2. Sort each loop-nest group based on axis (i.e., score)
  for (auto& group : computed_at_exprs) {
    sortGroup(group.second, scores);

    // Reorder expressions in a breadth-first order
    reorderGroupBreadthFirst(group.second, scores);
  }

  // 3. Merge non-root loop-nests into root loop-nests
  mergeNonRootGroupsIntoRootGroups(computed_at_exprs, target_map);

  // At this point, only root loop-nests (i.e., no computeAt'ed)
  // should exist.
  for (auto& group : computed_at_exprs) {
    // Guarantee only root loop-nests exist.
    TensorView* target = group.first;
    TORCH_INTERNAL_ASSERT(!target->hasComputeAt());
  }

  sanityCheck(exprs, reordered_exprs, scores, target_map, computed_at_exprs);

  mergeGroupsIntoSortedList(computed_at_exprs, reordered_exprs);

  // Reordering completed. Reordered exprs exist in reordered_exprs.

  TORCH_INTERNAL_ASSERT(exprs.size() == reordered_exprs.size());
  return reordered_exprs;
}

} // namespace

// Generate the loop nest structure and place it in lowered_exprs_
void LoopNestGenerator::generate(const std::vector<Expr*>& exprs) {
  FusionGuard fg(fusion_);

  TORCH_INTERNAL_ASSERT(lowered_exprs_.empty());

  // Identify all shared memory TensorViews
  // TODO: Make function to get all used TensorViews / used Vals
  for (auto v : fusion_->vals()) {
    if (v->getValType().value() == ValType::TensorView) {
      if (v->as<TensorView>()->getMemoryType() == MemoryType::Shared) {
        smem_.insert({v, false});
      }
    }
  }

  // Process the carefully ordered expressions
  for (const auto* expr : reorderExprsForComputeAt(exprs)) {
    handle(expr);
  }

  // Insert Dynamic Shared Memory at beginning of kernel
  for (auto smem_alloc : dynamic_smem_) {
    lowered_exprs_.insert(lowered_exprs_.begin(), smem_alloc);
  }
}

void LoopNestGenerator::cleanSharedMemory() {
  for (auto& item : smem_) {
    item.second = false;
  }
}

void LoopNestGenerator::modifySharedMemory(Val* key) {
  auto it = smem_.find(key);
  if (it != smem_.end()) {
    it->second = true;
  }
}

bool LoopNestGenerator::isModifiedSharedMemory(Val* key) const {
  auto it = smem_.find(key);
  if (it != smem_.end()) {
    return it->second;
  }
  return false;
}

} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
