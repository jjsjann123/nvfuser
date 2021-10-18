#include <torch/csrc/jit/codegen/cuda/scheduler/registry.h>
#include <torch/csrc/jit/codegen/cuda/scheduler/utils.h>

#include <torch/csrc/jit/codegen/cuda/compute_at_map.h>
#include <torch/csrc/jit/codegen/cuda/expr_evaluator.h>
#include <torch/csrc/jit/codegen/cuda/instrumentation.h>
#include <torch/csrc/jit/codegen/cuda/ir_utils.h>
#include <torch/csrc/jit/codegen/cuda/root_domain_map.h>
#include <torch/csrc/jit/codegen/cuda/transform_replay.h>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {
namespace scheduler_utils {

// Returns number of "valid" dimensions. e.g. if tv has
// [I1, R2, I3, I4, R3{1}]
// where R3{1} is in dont_merge, resulting domain should be:
// [I1, I3*I4, R2, R3{1}] with return value 3
//
// if tv has
// [R1, I2, R3, I4, R4, R5{1}, R6{1}]
//  where R5{1} and R6{1} are in dont_merge, resulting domain should be:
// [I2*I4, R1*R3, R4, R5{1}, R6{1}]
// with return value 3
size_t merge_3d(
    TensorView* tv,
    const std::unordered_set<IterDomain*>& dont_merge) {
  bool active_is_reduction = false;
  bool first_dim = true;
  int prev_i = -1;

  for (int i = static_cast<int>(tv->nDims()) - 1; i >= 0; i--) {
    if (dont_merge.count(tv->axis(i))) {
      continue;
    }

    if (first_dim) {
      active_is_reduction = tv->axis(i)->isReduction();
      prev_i = i;
      first_dim = false;
    } else {
      if (tv->axis(i)->isReduction() != active_is_reduction) {
        break;
      }
      tv->merge(i, prev_i);
      prev_i = i;
    }
  }

  if (prev_i == -1) {
    // Zero dimensional
    return 0;
  }

  // put inner most dimension as last dimension
  tv->reorder({{prev_i, -1}});
  active_is_reduction = false;
  first_dim = true;
  prev_i = -1;

  for (int i = static_cast<int>(tv->nDims()) - 2; i >= 0; i--) {
    auto id = tv->axis(i);
    if (dont_merge.count(id)) {
      continue;
    }

    if (first_dim) {
      active_is_reduction = id->isReduction();
      prev_i = i;
      first_dim = false;
    } else if (id->isReduction() == active_is_reduction) {
      tv->merge(i, prev_i);
      prev_i = i;
    }
  }

  // put second dimension as second to last dimension
  if (prev_i == -1) {
    // One dimensional, put merged dimension as first
    tv->reorder({{-1, 0}});
    return 1;
  } else {
    // put new dimension as second to last
    tv->reorder({{prev_i, -2}});
  }

  active_is_reduction = false;
  first_dim = true;
  prev_i = -1;

  for (int i = static_cast<int>(tv->nDims()) - 3; i >= 0; i--) {
    if (dont_merge.count(tv->axis(i))) {
      continue;
    }

    if (first_dim) {
      active_is_reduction = tv->axis(i)->isReduction();
      prev_i = i;
      first_dim = false;
    } else if (tv->axis(i)->isReduction() == active_is_reduction) {
      tv->merge(i, prev_i);
      prev_i = i;
    }
  }

  // put third dimension as second to last dimension
  if (prev_i == -1) {
    // Two dimensional, put merged dimensions first
    tv->reorder({{-1, 0}, {-2, 1}});
    // [outer, inner, dont_merge...]
    if (tv->axis(0)->isReduction()) {
      // put reductions as second axis
      tv->reorder({{0, 1}, {1, 0}});
    }
    return 2;
  } else {
    // put new dimension as third to last
    tv->reorder({{prev_i, -3}});
    // Stable sort to have iteration domains first, then reduction
    if (tv->axis(0)->isReduction() && !tv->axis(1)->isReduction()) {
      tv->reorder({{0, 1}, {1, 0}});
    }
    if (tv->axis(1)->isReduction() && !tv->axis(2)->isReduction()) {
      tv->reorder({{1, 2}, {2, 1}});
    }
    if (tv->axis(0)->isReduction() && !tv->axis(1)->isReduction()) {
      tv->reorder({{0, 1}, {1, 0}});
    }
    return 3;
  }
}

size_t mergeReduction(
    TensorView* tv,
    const std::unordered_set<IterDomain*>& dont_merge) {
  int prev_i = -1;
  size_t num_merged = 0;
  for (int i = static_cast<int>(tv->nDims()) - 1; i >= 0; i--) {
    if (!tv->axis(i)->isReduction() || dont_merge.count(tv->axis(i))) {
      continue;
    }
    if (prev_i == -1) {
      prev_i = i;
    } else {
      tv->merge(i, prev_i);
      prev_i = i;
      num_merged++;
    }
  }
  if (prev_i != 0) {
    tv->reorder({{prev_i, 0}});
  }

  return prev_i == -1 ? 0 : num_merged + 1;
}

size_t mergeNonReduction(
    TensorView* tv,
    const std::unordered_set<IterDomain*>& dont_merge) {
  int prev_i = -1;
  size_t num_merged = 0;
  if (tv->nDims() == 0) {
    return 0;
  }
  for (int i = static_cast<int>(tv->nDims()) - 1; i >= 0; i--) {
    if (tv->axis(i)->isReduction() || dont_merge.count(tv->axis(i))) {
      continue;
    }
    if (prev_i == -1) {
      prev_i = i;
    } else {
      tv->merge(i, prev_i);
      prev_i = i;
      num_merged++;
    }
  }
  if (prev_i != -1) {
    tv->reorder({{prev_i, 0}});
  }

  return prev_i == -1 ? 0 : num_merged + 1;
}

void parallelizeAllLike(
    TensorView* reference_tv,
    const std::vector<TensorView*>& all_tvs) {
  FusionGuard fg(reference_tv->fusion());

  // Use loop map as that is the most permissive.
  auto ca_loop_map = ComputeAtMap(ComputeAtMap::MappingMode::LOOP);
  ca_loop_map.build(FusionGuard::getCurFusion());
  for (auto id : reference_tv->domain()->domain()) {
    ca_loop_map.getConcreteMappedID(id)->parallelize(id->getParallelType());
    if (id->hasPaddingToMultipleOfWarp()) {
      TORCH_INTERNAL_ASSERT(id->getMaybeSizeAfterPadding().has_value());
      ca_loop_map.getConcreteMappedID(id)->padToMultipleOfWarp(
          id->getMaybeSizeAfterPadding().value());
    }
  }

  for (auto tv : all_tvs) {
    if (tv->isFusionInput()) {
      continue;
    }
    for (const auto i : c10::irange(tv->domain()->domain().size())) {
      tv->axis(i)->parallelize(
          ca_loop_map.getConcreteMappedID(tv->axis(i))->getParallelType());
    }
  }
}

void computeAtInputs(TensorView* consumer, int pos, ComputeAtMode mode) {
  for (auto inp_tv : ir_utils::inputTvsOf(consumer)) {
    inp_tv->computeAt(consumer, pos, mode);
  }
}

void computeWithOutputs(TensorView* producer, int pos, ComputeAtMode mode) {
  for (auto out_tv : ir_utils::outputTvsOf(producer)) {
    producer->computeWith(out_tv, pos, mode);
  }
}

PersistentBufferInfo persistentBuffers(Fusion* fusion) {
  FusionGuard fg(fusion);

  PersistentBufferInfo info;

  ComputeAtRootDomainMap root_map;
  root_map.build();

  auto all_tvs = ir_utils::allTvs(fusion);

  for (auto producer : all_tvs) {
    bool mappable = true;
    auto consumers = ir_utils::consumerTvsOf(producer);
    if (consumers.empty()) {
      continue;
    }

    auto mappable_roots =
        root_map.getMappableDims(producer->domain(), consumers[0]->domain());

    auto p_root = producer->getMaybeRFactorDomain();

    for (auto p_root_id : p_root) {
      if (p_root_id->isReduction()) {
        continue;
      }
      if (!mappable_roots.count(p_root_id)) {
        mappable = false;
        info.unmappable_dims.emplace(p_root_id);
      }
    }

    if (!mappable) {
      info.buffers.push_back(producer);
    }
  }
  return info;
}

TvProperties getProperties(
    Fusion* fusion,
    SchedulerRuntimeInfo& runtime_info,
    TensorView* tv) {
  FusionGuard fg(fusion);

  TORCH_INTERNAL_ASSERT(tv != nullptr);

  auto root_dom = tv->getRootDomain();
  bool fastest_dim_reduction = true;

  // Is there a non trivial reduction on the inner most dimension or is there an
  // iteration domain.
  for (size_t i = root_dom.size(); i > 0; i--) {
    if (root_dom[i - 1]->isBroadcast() ||
        root_dom[i - 1]->isTrivialReduction()) {
      continue;
    } else if (root_dom[i - 1]->isReduction()) {
      fastest_dim_reduction = true;
      break;
    } else {
      fastest_dim_reduction = false;
      break;
    }
  }

  // Tracks the dimensionality of the problem starts on inner most dim and works
  // outward
  int64_t dimensionality = 1;
  // Initialize for dimensionality analysis
  bool cur_dim_is_reduction = fastest_dim_reduction;
  // Compute the size of the inner most dimension
  int64_t inner_most_dimension_numel = 1;

  // Start from the inner most dimension, and work outwards. If this is a 3D
  // pattern, i.e. theres a pattern like [r0, r1, i2, r3] or [i0, r1, r2, i3,
  // i4] then compute the inner most dimension to compute separately.
  for (size_t i = root_dom.size(); i > 0; i--) {
    auto id = root_dom[i - 1];
    if (id->isBroadcast() || id->isTrivialReduction()) {
      continue;
    }
    if (id->isReduction() != cur_dim_is_reduction) {
      dimensionality++;
      cur_dim_is_reduction = !cur_dim_is_reduction;
    } else if (dimensionality == 1) {
      auto inferred_val =
          runtime_info.expressionEvaluator().evaluate(id->extent());
      TORCH_INTERNAL_ASSERT(
          inferred_val.has_value(), "Error inferring reduction size.");
      inner_most_dimension_numel =
          inner_most_dimension_numel * inferred_val.value();
    }
  }

  // Non reduction element count
  int64_t total_iteration_numel = 1;
  // Reduction element count
  int64_t total_reduction_numel = 1;

  for (auto id : tv->getRootDomain()) {
    auto inferred_val =
        runtime_info.expressionEvaluator().evaluate(id->extent());
    TORCH_INTERNAL_ASSERT(
        inferred_val.has_value(),
        "Error inferring dimensions of reduction fusion.");
    if (id->isReduction()) {
      total_reduction_numel *= inferred_val.value();
    } else {
      total_iteration_numel *= inferred_val.value();
    }
  }

  TvProperties properties;
  properties.total_reduction_numel = total_reduction_numel;
  properties.total_iteration_numel = total_iteration_numel;
  properties.fastest_dim_reduction = fastest_dim_reduction;
  properties.inner_most_dimension_numel = inner_most_dimension_numel;
  properties.dimensionality = dimensionality;

  return properties;
}

void computeAtBetween(
    const std::vector<TensorView*>& producers,
    const std::vector<TensorView*>& overall_consumers,
    int pos,
    ComputeAtMode mode,
    std::unordered_set<IterDomain*> mapped_to_trivial_reduction) {
  for (auto producer : producers) {
    // Figure out what's between producer and overall_consumers, will not give
    // back any consumers that are not downstream from producer
    auto all_vals_between = DependencyCheck::getAllValsBetween(
        {producer}, {overall_consumers.begin(), overall_consumers.end()});

    std::unordered_set<Val*> all_vals_between_set(
        all_vals_between.begin(), all_vals_between.end());

    for (auto consumer : overall_consumers) {
      if (all_vals_between_set.count(consumer)) {
        // The way we generate producers and consumers is that we inch away from
        // inputs/outputs. There's a chance we could meet in the middle.
        if (producer == consumer) {
          continue;
        }

        auto pos_it = std::find_if(
            consumer->domain()->domain().begin(),
            consumer->domain()->domain().end(),
            [&mapped_to_trivial_reduction](IterDomain* id) {
              return mapped_to_trivial_reduction.count(id);
            });

        pos = pos_it == consumer->domain()->domain().end()
            ? pos
            : std::min(
                  (int)std::distance(
                      consumer->domain()->domain().begin(), pos_it) +
                      1,
                  (pos < 0 ? pos + (int)consumer->nDims() : pos));
        // Assume we don't want to reset computeAt on tensors that have already
        // performed it.
        producer->computeAt(consumer, pos, mode);
      }
    }
  }
}

namespace {

std::unique_ptr<HeuristicCompileTime::ScopedPersistenceFactorMap>
getScopePersistenceFactors(
    Fusion* fusion,
    PersistentBufferInfo& persistent_buffers) {
  auto new_persistent_factor_map_ptr =
      std::make_unique<HeuristicCompileTime::ScopedPersistenceFactorMap>();
  auto& new_persistent_factor_map = *new_persistent_factor_map_ptr;

  for (auto tv : persistent_buffers.buffers) {
    auto& consumer_tv_to_factor_map_ptr = new_persistent_factor_map[tv];
    consumer_tv_to_factor_map_ptr =
        std::make_unique<HeuristicCompileTime::ValToFactorMap>();
    auto& consumer_tv_to_factor_map = *consumer_tv_to_factor_map_ptr;

    // All expressions between tv and its consumers must have tv's persistent
    // buffer allocated. This is an optimistic view on how many registers we
    // need allocated in the kernel, since if we ordered two persistent
    // buffers that are completely independent to somehow overlap with
    // eachother we would assume we wouldn't need those two buffers active at
    // the same time, even though they would be.
    //
    // Unfortunately this limitation is hard to work around as we would have
    // to actually generate the kernel before we know if it would fit
    // persistently in registers. In practice, though, this should not happen
    // as inlining loop structures where the persistent buffer is used should
    // prevent muiltiple persistent buffers from being merged togther if not
    // necessary.
    auto consumers_of_tv = ir_utils::consumerTvsOf(tv);
    for (auto val : DependencyCheck::getAllValsBetween(
             {tv}, {consumers_of_tv.begin(), consumers_of_tv.end()})) {
      // Persistent normalization kernels imply that all persistent buffers
      // have the same dimensionality. Assume if a persistent buffer is
      // consumed by another we can alias and reuse the memory.
      if (val == tv) {
        continue;
      }

      if (consumer_tv_to_factor_map.count(val)) {
        consumer_tv_to_factor_map.at(val) += 1;
      } else {
        consumer_tv_to_factor_map[val] = 1;
      }
    }
  }
  return new_persistent_factor_map_ptr;
}

} // namespace

int64_t persistentBufferSize(
    Fusion* fusion,
    SchedulerRuntimeInfo& runtime_info,
    PersistentBufferInfo& persistent_buffers,
    HeuristicSummary* data_cache) {
  FUSER_PERF_SCOPE("scheduler_utils::persistentBufferSize");

  if (persistent_buffers.buffers.empty()) {
    return 0;
  }

  int64_t persistent_buffer_size = 0;

  auto persistent_buffer_info_entry =
      HeuristicSummaryEntry<HeuristicCompileTime::ScopePersistentFactorInfo>(
          data_cache, [&fusion, &persistent_buffers]() {
            return getScopePersistenceFactors(fusion, persistent_buffers);
          });

  auto& scoped_persistence_factor = persistent_buffer_info_entry.get();

  // Runtime: convert the persistent factor to actual values
  std::unordered_map<Val*, int64_t> scoped_persistence;

  for (auto tv : persistent_buffers.buffers) {
    int64_t tv_persistent_numel = -1;
    for (auto id : tv->getMaybeRFactorDomain()) {
      if (id->isReduction() || id->isBroadcast()) {
        continue;
      }
      // Unmappable dimensions are those that we cannot inline into other
      // tensor views. So they're the ones that need to be persistent.
      if (!persistent_buffers.unmappable_dims.count(id)) {
        continue;
      }

      auto id_size = runtime_info.expressionEvaluator().evaluate(id->extent());
      TORCH_INTERNAL_ASSERT(
          id_size.has_value(),
          "Cannot generate heuristics if we don't have input information.");
      if (tv_persistent_numel == -1) {
        tv_persistent_numel = id_size.value();
      } else {
        tv_persistent_numel *= id_size.value();
      }
    }

    persistent_buffer_size =
        tv_persistent_numel * dataTypeSize(tv->getDataType().value());

    // Look up the contribution part from the cached matrix:
    auto scoped_factor_it = scoped_persistence_factor.find(tv);
    if (scoped_factor_it != scoped_persistence_factor.end()) {
      // now looking at scoped_persistence_factor[tv]
      for (auto val_to_factor_it : *(scoped_factor_it->second)) {
        // (val_to_factor_it) is (val, factor)
        int64_t persistent_buffer_size_contribution =
            persistent_buffer_size * val_to_factor_it.second;

        //  try to write factor * persistent_buffer_size into
        //  scoped_persistence[val]
        auto val_it = scoped_persistence.find(val_to_factor_it.first);
        if (val_it == scoped_persistence.end()) {
          scoped_persistence[val_to_factor_it.first] =
              persistent_buffer_size_contribution;
        } else {
          val_it->second += persistent_buffer_size_contribution;
        }
      }
    }
  }

  // Find the maximum persistent buffer use
  int64_t max_persistence_size = 0;
  for (auto persistent_entry : scoped_persistence) {
    max_persistence_size =
        std::max(max_persistence_size, persistent_entry.second);
  }

  return max_persistence_size;
}

std::unordered_set<IterDomain*> getTrivialReductionMap(Fusion* fusion) {
  auto all_tvs = ir_utils::allTvs(fusion);
  std::unordered_set<IterDomain*> mapped_to_trivial_reduction;
  for (auto tv : all_tvs) {
    // root domain vs domain shouldn't matter as at this point we shouldn't have
    // any transformations.
    for (auto id : tv->getRootDomain()) {
      if (id->isTrivialReduction()) {
        mapped_to_trivial_reduction.emplace(id);
      }
    }
  }

  if (!mapped_to_trivial_reduction.empty()) {
    // Use the loop map as that is the most permissive
    auto ca_index_map = ComputeAtMap(ComputeAtMap::MappingMode::LOOP);
    ca_index_map.build(fusion);
    // Make a copy we need to check mappings of all
    auto trivial_ids = mapped_to_trivial_reduction;
    for (auto tv : all_tvs) {
      for (auto id : tv->getRootDomain()) {
        if (!id->extent()->isOneInt()) {
          continue;
        }
        if (std::any_of(
                trivial_ids.begin(),
                trivial_ids.end(),
                [&ca_index_map, &id](IterDomain* trivial_id) {
                  return ca_index_map.areMapped(id, trivial_id);
                })) {
          mapped_to_trivial_reduction.emplace(id);
        }
      }
    }
  }
  return mapped_to_trivial_reduction;
}

std::pair<bool, bool> canonicalDimReduction(
    Fusion* fusion,
    TensorView* tv,
    bool schedule_3D) {
  std::unordered_set<IterDomain*> mapped_to_trivial_reduction =
      getTrivialReductionMap(fusion);

  TORCH_INTERNAL_ASSERT(tv != nullptr);

  if (!schedule_3D) {
    // We coalesce all reduction axes to the right;
    bool has_red_axis = mergeReduction(tv, mapped_to_trivial_reduction) > 0;

    bool has_iter_axis = mergeNonReduction(tv, mapped_to_trivial_reduction) > 0;
    return {has_iter_axis, has_red_axis};
  } else {
    TORCH_INTERNAL_ASSERT(
        merge_3d(tv, mapped_to_trivial_reduction) == 3,
        "Tried 3D merge, but result is not 3D.");
    return {true, true};
  }
}

std::vector<TensorView*> getReductionTvs(Fusion* fusion) {
  auto all_tvs = ir_utils::allTvs(fusion);
  std::vector<TensorView*> reduction_tvs;
  for (auto tv : all_tvs) {
    if (!tv->isFusionInput() &&
        std::any_of(
            tv->domain()->domain().begin(),
            tv->domain()->domain().end(),
            [](IterDomain* id) {
              return id->isReduction() && !id->isTrivialReduction();
            })) {
      reduction_tvs.emplace_back(tv);
    }
  }

  // Remove multi outputs from reduction tensor views
  std::unordered_set<Expr*> seen_reduction_exprs;
  reduction_tvs.erase(
      std::remove_if(
          reduction_tvs.begin(),
          reduction_tvs.end(),
          [&seen_reduction_exprs](TensorView* tv) {
            TORCH_INTERNAL_ASSERT(
                tv->definition() != nullptr,
                "Somehow a tensor view without a definition but a reduction snuck into the scheduler reduction list.");
            if (!seen_reduction_exprs.emplace(tv->definition()).second) {
              return true;
            }
            return false;
          }),
      reduction_tvs.end());
  return reduction_tvs;
}

// Reset inputs and outputs to global memory, everything else to local.
void clearMemorySpace(Fusion* fusion) {
  for (auto tv : ir_utils::allTvs(fusion)) {
    if (tv->isFusionInput() || tv->isFusionOutput()) {
      tv->setMemoryType(MemoryType::Global);
    } else {
      tv->setMemoryType(MemoryType::Local);
    }
  }
}

// Returns cached after tensors of the fusion inputs if unrolled. Otherwise
// return empty vector.
std::vector<TensorView*> cacheInputs(Fusion* fusion, bool unroll) {
  if (!unroll) {
    return {};
  }

  std::vector<TensorView*> cached_inputs;
  // If we're going to unroll, make a cache of the inputs
  auto in_tvs = ir_utils::filterByType<TensorView>(fusion->inputs());
  for (auto tv : in_tvs) {
    if (tv->uses().empty()) {
      continue;
    }
    auto cached_tv = tv->cache_after();
    cached_inputs.emplace_back(cached_tv);
  }
  return cached_inputs;
}

// Returns the pairs of <cache of each fusion output, corresponding output> for
// all outputs.
std::vector<std::pair<TensorView*, TensorView*>> cacheAndForkOutputs(
    Fusion* fusion,
    bool unroll) {
  std::vector<std::pair<TensorView*, TensorView*>> cached_outputs;
  // For intermediate outputs, apply cache_fork
  for (const auto output :
       ir_utils::filterByType<TensorView>(fusion->outputs())) {
    if (output->definition() == nullptr) {
      continue;
    }
    if (!output->uses().empty()) {
      auto cached_output = output->cache_fork();
      cached_outputs.emplace_back(std::make_pair(output, cached_output));
    } else if (unroll) {
      auto cached_output = output->cache_before();
      cached_outputs.emplace_back(std::make_pair(cached_output, output));
    }
  }
  return cached_outputs;
}

FindAllMappedDims::FindAllMappedDims(TensorView* from, IterDomain* id)
    : starting_tv(from), starting_id(id) {
  std::deque<TensorView*> to_visit{starting_tv};
  std::unordered_set<TensorView*> visited;
  mapped_ids.emplace(std::make_pair(starting_tv, starting_id));

  // Propagate mapping of id
  while (!to_visit.empty()) {
    auto tv = to_visit.front();
    to_visit.pop_front();

    if (!visited.emplace(tv).second) {
      continue;
    }

    auto tv_id = mapped_ids.at(tv);

    for (auto consumer_tv : ir_utils::consumerTvsOf(tv)) {
      if (visited.find(consumer_tv) != visited.end()) {
        continue;
      }

      if (mapped_ids.find(consumer_tv) != mapped_ids.end()) {
        continue;
      }

      PairwiseRootDomainMap root_map(tv, consumer_tv);
      auto p2c_map =
          root_map.mapProducerToConsumer(tv->domain(), consumer_tv->domain());

      auto c_it = p2c_map.find(tv_id);
      if (c_it != p2c_map.end()) {
        mapped_ids.emplace(std::make_pair(consumer_tv, c_it->second));
        to_visit.emplace_back(consumer_tv);
      }
    }

    for (auto producer_tv : ir_utils::producerTvsOf(tv)) {
      if (visited.find(producer_tv) != visited.end()) {
        continue;
      }

      if (mapped_ids.find(producer_tv) != mapped_ids.end()) {
        continue;
      }

      PairwiseRootDomainMap root_map(producer_tv, tv);
      auto c2p_map =
          root_map.mapConsumerToProducer(tv->domain(), producer_tv->domain());
      auto p_it = c2p_map.find(tv_id);
      if (p_it != c2p_map.end()) {
        mapped_ids.emplace(std::make_pair(producer_tv, p_it->second));
        to_visit.emplace_back(producer_tv);
      }
    }
  }
}

std::unordered_set<IterDomain*> FindAllMappedDims::from(
    TensorView* tv,
    IterDomain* id) {
  TORCH_INTERNAL_ASSERT(
      std::find_if(
          tv->getRootDomain().begin(),
          tv->getRootDomain().end(),
          [&id](IterDomain* root_id) { return root_id == id; }) !=
          tv->getRootDomain().end(),
      "Tried to map out ",
      id,
      " from TV ",
      tv,
      " to the rest of the fusion, but id does not belong to this tv.");

  FindAllMappedDims mapped_dims(tv, id);

  std::unordered_set<IterDomain*> mapped_id_set;
  for (auto entry : mapped_dims.mapped_ids) {
    mapped_id_set.emplace(entry.second);
  }
  return mapped_id_set;
}

bool hasInnerDim(
    TensorView* tv,
    std::unordered_set<IterDomain*> vector_dims,
    bool should_vectorize) {
  const auto& root_dom = TensorDomain::noBroadcasts(
      TensorDomain::noReductions(tv->getRootDomain()));

  // Don't vectorize 0-dim tensors
  if (root_dom.size() == 0) {
    return false;
  }

  auto inner_most_dim = root_dom[root_dom.size() - 1];

  // Make sure inner most dimension is in the vector_dim set
  if (vector_dims.count(inner_most_dim) == 0) {
    return false;
  }

  if (!should_vectorize) {
    return true;
  }

  auto root_pos_it = std::find_if(
      tv->getRootDomain().begin(),
      tv->getRootDomain().end(),
      [&inner_most_dim](IterDomain* id) { return inner_most_dim == id; });

  TORCH_INTERNAL_ASSERT(root_pos_it != tv->getRootDomain().end());
  auto inner_most_dim_pos =
      std::distance(tv->getRootDomain().begin(), root_pos_it);

  const auto& contiguity = tv->domain()->contiguity();

  TORCH_INTERNAL_ASSERT(contiguity.size() == tv->getRootDomain().size());

  // Don't vectorize if inner most dimension is not contiguous
  if (!contiguity[inner_most_dim_pos]) {
    return false;
  }

  return true;
}

std::vector<TensorView*> getInputsOutputsWithInnerDim(
    TensorView* reference_tv,
    bool can_vectorize) {
  if (reference_tv->nDims() == 0) {
    return {};
  }

  IterDomain* inner_most_id = nullptr;
  for (auto it = reference_tv->getRootDomain().rbegin();
       it != reference_tv->getRootDomain().rend();
       it++) {
    if ((*it)->isReduction() && reference_tv->isFusionInput()) {
      continue;
    }
    if ((*it)->isBroadcast()) {
      if (inner_most_id == nullptr) {
        inner_most_id = *it;
      }
      continue;
    }
    if ((*it)->isTrivialReduction()) {
      if (inner_most_id == nullptr) {
        inner_most_id = *it;
      }
      continue;
    }
    inner_most_id = *it;
    break;
  }

  if (inner_most_id == nullptr) {
    return {};
  }

  auto vectorizable_dims = FindAllMappedDims::from(reference_tv, inner_most_id);

  std::vector<TensorView*> vectorizable_tensors;

  for (auto input_tv :
       ir_utils::filterByType<TensorView>(reference_tv->fusion()->inputs())) {
    if (hasInnerDim(input_tv, vectorizable_dims, can_vectorize)) {
      vectorizable_tensors.push_back(input_tv);
    }
  }

  for (auto output_tv :
       ir_utils::filterByType<TensorView>(reference_tv->fusion()->outputs())) {
    if (hasInnerDim(output_tv, vectorizable_dims, can_vectorize)) {
      vectorizable_tensors.push_back(output_tv);
    }
  }

  return vectorizable_tensors;
}

std::vector<BroadcastMultiple> getBroadcastMultiples(TensorView* reference_tv) {
  auto fusion = reference_tv->fusion();
  FusionGuard fg(fusion);

  std::vector<BroadcastMultiple> multiples(
      reference_tv->getMaybeRFactorDomain().size());

  // All input or output tensor views
  std::vector<TensorView*> in_out_tvs;
  {
    auto inp_tvs = ir_utils::filterByType<TensorView>(fusion->inputs());
    in_out_tvs.insert(in_out_tvs.end(), inp_tvs.begin(), inp_tvs.end());
    auto out_tvs = ir_utils::filterByType<TensorView>(fusion->outputs());
    in_out_tvs.insert(in_out_tvs.end(), out_tvs.begin(), out_tvs.end());
  }

  // Shouldn't matter which compute at map we use
  auto ca_index_map = ComputeAtMap(ComputeAtMap::MappingMode::INDEX);
  ca_index_map.build(fusion);

  auto ref_root_domain = reference_tv->getMaybeRFactorDomain();

  // Map all inputs and output domains to reference tv domains
  for (auto in_out_tv : in_out_tvs) {
    std::vector<bool> mapped_axes(ref_root_domain.size(), false);

    auto in_out_tv_domain = in_out_tv->getRootDomain();
    auto in_out_tv_domain_list = std::list<IterDomain*>(
        in_out_tv_domain.begin(), in_out_tv_domain.end());

    for (const auto ref_i : c10::irange(ref_root_domain.size())) {
      auto ref_id = ref_root_domain[ref_i];

      // If reference id is broadcast or reduction
      if (ref_id->isBroadcast() || ref_id->isReduction()) {
        continue;
      }
      auto map_it = std::find_if(
          in_out_tv_domain_list.begin(),
          in_out_tv_domain_list.end(),
          [&ref_id, &ca_index_map](IterDomain* in_out_tv_id) {
            return ca_index_map.areMapped(in_out_tv_id, ref_id);
          });

      if (map_it == in_out_tv_domain_list.end()) {
        continue;
      }

      // If input/output id is broadcast or reduction
      if ((*map_it)->isBroadcast() || (*map_it)->isReduction()) {
        continue;
      }

      mapped_axes[ref_i] = true;
      in_out_tv_domain_list.erase(map_it);
    }

    // For each break point position if there an lhs or rhs multiple based on
    // this tensor add it to the global multiplier
    {
      bool rhs = false;
      bool lhs = false;
      auto dtype_size = dataTypeSize(in_out_tv->getDataType().value());
      for (size_t mapped_axes_i = 0; mapped_axes_i < mapped_axes.size();
           mapped_axes_i++) {
        auto lhs_i = mapped_axes_i;
        auto rhs_i = mapped_axes.size() - 1 - mapped_axes_i;

        if (lhs) {
          multiples[lhs_i].lhs_multiple += dtype_size;
        } else if (mapped_axes[lhs_i]) {
          lhs = true;
        }

        if (rhs || mapped_axes[rhs_i]) {
          multiples[rhs_i].rhs_multiple += dtype_size;
          rhs = true;
        }
      }
    }
  }

  return multiples;
}

} // namespace scheduler_utils
} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
