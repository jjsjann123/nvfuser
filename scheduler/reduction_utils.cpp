#include <torch/csrc/jit/codegen/cuda/scheduler/reduction_utils.h>

#include <torch/csrc/jit/codegen/cuda/expr_evaluator.h>
#include <torch/csrc/jit/codegen/cuda/ir_utils.h>
#include <torch/csrc/jit/codegen/cuda/scheduler/registry.h>
#include <torch/csrc/jit/codegen/cuda/scheduler/utils.h>
#include <torch/csrc/jit/codegen/cuda/transform_replay.h>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

namespace reduction_scheduler_utils {

TensorView* scheduleReductionTV(
    const ReductionParams& rparams,
    TensorView* reduction_tv,
    bool has_iter_axis) {
  // Outer and inner reduction axis is relative. Outer reduce axis is only valid
  // in 3D scheduling. Otherwise inner_reduce_axis is the only reduction axis.
  // Inner here though is only relative to the other axis. When
  // rparams.fastest_dim == false, the reduction axis is logically outside the
  // iteration axis.
  const int iter_axis = 0;
  const int outer_reduce_axis = rparams.schedule_3D ? 1 : 0;
  const int inner_reduce_axis = rparams.schedule_3D ? 2 : has_iter_axis ? 1 : 0;

  TORCH_INTERNAL_ASSERT(
      (int)reduction_tv->nDims() >
          std::max(iter_axis, std::max(outer_reduce_axis, inner_reduce_axis)),
      "Issue in scheduling reduction tv, expecting >",
      std::max(iter_axis, std::max(outer_reduce_axis, inner_reduce_axis)),
      " dimensions, but found ",
      reduction_tv->nDims());

  TORCH_INTERNAL_ASSERT(
      !(rparams.fastest_dim && rparams.vectorize_iter_dom),
      "Cannot vectorize iteration domain on inner reductions.");

  TORCH_INTERNAL_ASSERT(
      !(!rparams.fastest_dim && rparams.vectorize_inner_reduction),
      "Cannot vectorize reduction domain on outer reductions.");

  TORCH_INTERNAL_ASSERT(
      !(rparams.cross_grid_inner_reduce && rparams.persistent_kernel),
      "Grid reductions not implemented yet for persistent kernels.");

  TORCH_INTERNAL_ASSERT(
      !(rparams.multiple_reds_per_blk && !has_iter_axis),
      "Multiple reductions requires an iter domain, but one wasn't found.");

  TORCH_INTERNAL_ASSERT(
      !(rparams.cross_grid_inner_reduce && rparams.unroll_iter_dom),
      "Unrolling on iter domain not supported with cross grid reductions.");

  TORCH_INTERNAL_ASSERT(
      !(rparams.unroll_iter_dom && !has_iter_axis),
      "Unrolling on iter domain requires an iter domain.");

  // Inner reduction axis:
  if (rparams.unroll_inner_reduction) {
    if (rparams.persistent_kernel) {
      if (rparams.vectorize_inner_reduction) {
        reduction_tv->split(
            inner_reduce_axis, rparams.batches_per_block, false);
        reduction_tv->split(
            inner_reduce_axis + 1, rparams.unroll_factor_inner_reduction);

        reduction_tv->axis(inner_reduce_axis + 1)
            ->parallelize(rparams.block_dim_inner_reduction);
        reduction_tv->axis(inner_reduce_axis + 2)
            ->parallelize(ParallelType::Vectorize);
      } else {
        reduction_tv->split(
            inner_reduce_axis,
            rparams.batches_per_block * rparams.unroll_factor_inner_reduction,
            false);
        reduction_tv->split(
            inner_reduce_axis, rparams.unroll_factor_inner_reduction);

        reduction_tv->axis(inner_reduce_axis + 1)
            ->parallelize(ParallelType::Unroll);
        reduction_tv->axis(inner_reduce_axis + 2)
            ->parallelize(rparams.block_dim_inner_reduction);
      }
    } else {
      if (isParallelTypeThread(rparams.block_dim_inner_reduction)) {
        if (rparams.vectorize_inner_reduction) {
          reduction_tv->split(
              inner_reduce_axis, rparams.unroll_factor_inner_reduction);
          reduction_tv->split(
              inner_reduce_axis,
              NamedScalar::getParallelDim(rparams.block_dim_inner_reduction));

          reduction_tv->axis(inner_reduce_axis + 2)
              ->parallelize(ParallelType::Vectorize);
          reduction_tv->axis(inner_reduce_axis + 1)
              ->parallelize(rparams.block_dim_inner_reduction);
        } else {
          reduction_tv->split(
              inner_reduce_axis,
              NamedScalar::getParallelDim(rparams.block_dim_inner_reduction));
          reduction_tv->split(
              inner_reduce_axis, rparams.unroll_factor_inner_reduction);

          reduction_tv->axis(inner_reduce_axis + 1)
              ->parallelize(ParallelType::Unroll);
          reduction_tv->axis(inner_reduce_axis + 2)
              ->parallelize(rparams.block_dim_inner_reduction);
        }
      } else {
        // Inner reduction is not parallelized, but is unrolled or vectorized:
        reduction_tv->split(
            inner_reduce_axis, rparams.unroll_factor_inner_reduction);
        reduction_tv->axis(inner_reduce_axis + 1)
            ->parallelize(
                rparams.vectorize_inner_reduction ? ParallelType::Vectorize
                                                  : ParallelType::Unroll);
      }
    }

    // Unswitch axis which gives us finer control on allocations with
    // unrolling
    reduction_tv->split(inner_reduce_axis, 1);
    reduction_tv->axis(inner_reduce_axis + 1)
        ->parallelize(ParallelType::Unswitch);
  } else {
    // Parallelize reduction axis, don't unroll it0
    if (rparams.cross_block_inner_reduce) {
      if (rparams.persistent_kernel) {
        reduction_tv->split(
            inner_reduce_axis, rparams.batches_per_block, false);
        reduction_tv->axis(inner_reduce_axis + 1)
            ->parallelize(rparams.block_dim_inner_reduction);
      } else {
        reduction_tv->split(
            inner_reduce_axis,
            NamedScalar::getParallelDim(rparams.block_dim_inner_reduction));
        reduction_tv->axis(inner_reduce_axis + 1)
            ->parallelize(rparams.block_dim_inner_reduction);
      }
    } else {
      // No parallelization on reduction dim, fake an unswitch axis for
      // rfactor
      reduction_tv->split(inner_reduce_axis, 1);
      reduction_tv->axis(inner_reduce_axis + 1)
          ->parallelize(ParallelType::Unswitch);
    }
  }

  if (rparams.cross_grid_inner_reduce) {
    reduction_tv->split(
        inner_reduce_axis,
        NamedScalar::getParallelDim(rparams.grid_dim_inner_reduction),
        false);
    reduction_tv->axis(inner_reduce_axis)
        ->parallelize(rparams.grid_dim_inner_reduction);
  }

  // Outer reduction axis
  if (rparams.schedule_3D) {
    if (rparams.cross_grid_outer_reduce) {
      // Unsafe as we could be over the grid y dim limit, but this is 3D
      // scheduler so seems unlikely in practice
      reduction_tv->split(
          outer_reduce_axis,
          NamedScalar::getParallelDim(rparams.grid_dim_outer_reduction));
      reduction_tv->axis(outer_reduce_axis + 1)
          ->parallelize(rparams.grid_dim_outer_reduction);
    }
  }

  // Iteration domain
  if (has_iter_axis) {
    if (isParallelTypeThread(rparams.block_dim_iter_dom)) {
      if (rparams.vectorize_iter_dom) {
        reduction_tv->split(iter_axis, rparams.unroll_factor_iter_dom);
        reduction_tv->axis(iter_axis + 1)->parallelize(ParallelType::Vectorize);

        reduction_tv->split(
            iter_axis, NamedScalar::getParallelDim(rparams.block_dim_iter_dom));
        reduction_tv->axis(iter_axis + 1)
            ->parallelize(rparams.block_dim_iter_dom);
      } else {
        if ((rparams.fastest_dim && rparams.multiple_reds_per_blk) ||
            !rparams.fastest_dim) {
          reduction_tv->split(
              iter_axis,
              NamedScalar::getParallelDim(rparams.block_dim_iter_dom));
          reduction_tv->axis(iter_axis + 1)
              ->parallelize(rparams.block_dim_iter_dom);
        }
        if (rparams.unroll_iter_dom) {
          reduction_tv->split(iter_axis, rparams.unroll_factor_iter_dom);
          reduction_tv->axis(iter_axis + 1)->parallelize(ParallelType::Unroll);
        }
      }
    } else if (rparams.unroll_iter_dom) {
      // Iteration domain is not parallelized but it is unrolled or vectorized
      reduction_tv->split(iter_axis, rparams.unroll_factor_iter_dom);
      if (rparams.vectorize_iter_dom) {
        reduction_tv->axis(iter_axis + 1)->parallelize(ParallelType::Vectorize);
      } else {
        reduction_tv->axis(iter_axis + 1)->parallelize(ParallelType::Unroll);
      }
    }
    if (rparams.unroll_iter_dom) {
      reduction_tv->split(iter_axis, 1);
      reduction_tv->axis(iter_axis + 1)->parallelize(ParallelType::Unswitch);
    }

    if (rparams.fastest_dim && rparams.split_grid_dim_iter_dom) {
      reduction_tv->split(iter_axis, scheduler_utils::x_grid_limit);
      reduction_tv->axis(iter_axis + 1)->parallelize(rparams.grid_dim_iter_dom);
    } else {
      reduction_tv->axis(iter_axis)->parallelize(rparams.grid_dim_iter_dom);
    }
  }

  return sortAndRFactor(reduction_tv);
}

void multiReductionInliner(
    Fusion* fusion,
    const ReductionParams& rparams,
    TensorView* reduction_tv,
    TensorView* reference_tv,
    std::vector<TensorView*> reduction_tvs,
    std::vector<TensorView*> cached_inputs,
    std::vector<std::pair<TensorView*, TensorView*>> cached_outputs) {
  TransformPropagator::from(reference_tv);

  // Apply rfactor to all reductions if applicable
  std::vector<TensorView*> rfactor_tvs;

  if (reference_tv != reduction_tv) {
    std::vector<int> rfactor_axes;
    for (const auto i : c10::irange(reference_tv->nDims())) {
      if (reference_tv->axis((int)i)->isReduction() &&
          reference_tv->axis((int)i)->isRFactorProduct()) {
        rfactor_axes.push_back((int)i);
      }
    }

    for (auto reduction_tv_ : reduction_tvs) {
      if (reduction_tv_ == reduction_tv) {
        // The reduction tv
        rfactor_tvs.push_back(reference_tv);
        continue;
      } else {
        rfactor_tvs.push_back(
            ir_utils::rfactorHelper(reduction_tv_, rfactor_axes));
      }
    }

    TORCH_INTERNAL_ASSERT(
        reduction_tvs.size() == rfactor_tvs.size(),
        "Expected all reductions to contain rfactor.");
  }

  // Propagate parallelization
  scheduler_utils::parallelizeAllLike(reference_tv, ir_utils::allTvs(fusion));

  // Find iter domains that are mapped to a trivial reduction, these should
  // never be inlined.
  std::unordered_set<IterDomain*> mapped_to_trivial_reduction =
      scheduler_utils::getTrivialReductionMap(fusion);

  bool unroll = rparams.unroll_inner_reduction || rparams.unroll_iter_dom;

  bool vectorize =
      rparams.vectorize_inner_reduction || rparams.vectorize_iter_dom;

  if (unroll) {
    // Inline Input caches to their consumers outside unswitched/vectorization
    // position Inline consumers of input caches to rfactor tensors

    // Mark which tensor views are actual input caches to leave vectorization on
    // them
    std::unordered_set<TensorView*> keep_unrolled;

    std::vector<TensorView*> compute_from;

    // Grab all tensor views that should be vectorized
    auto vecotrizable_inputs_outputs =
        scheduler_utils::getInputsOutputsWithInnerDim(reference_tv, true);

    // Inputs to cache
    for (auto cached_input : cached_inputs) {
      auto consumers_of_input_cache = ir_utils::consumerTvsOf(cached_input);
      for (auto consumer : consumers_of_input_cache) {
        auto unswitch_it = std::find_if(
            consumer->domain()->domain().begin(),
            consumer->domain()->domain().end(),
            [&mapped_to_trivial_reduction](IterDomain* id) {
              return id->getParallelType() == ParallelType::Unswitch ||
                  id->getParallelType() == ParallelType::Unroll ||
                  id->getParallelType() == ParallelType::Vectorize ||
                  id->getParallelType() == ParallelType::MisalignedVectorize ||
                  mapped_to_trivial_reduction.count(id);
            });
        auto unswitch_pos = unswitch_it == consumer->domain()->domain().end()
            ? -1
            : std::distance(consumer->domain()->domain().begin(), unswitch_it) +
                1;

        cached_input->computeAt(
            consumer, unswitch_pos, ComputeAtMode::BestEffort);
        compute_from.push_back(consumer);

        if (vectorize) {
          auto producer_tvs = ir_utils::producerTvsOf(cached_input);
          if (producer_tvs.size() == 1 &&
              std::find(
                  vecotrizable_inputs_outputs.begin(),
                  vecotrizable_inputs_outputs.end(),
                  producer_tvs[0]) != vecotrizable_inputs_outputs.end()) {
            keep_unrolled.emplace(cached_input);
          }
        } else {
          keep_unrolled.emplace(cached_input);
        }
      }
    }

    // Inline output caches into outputs
    std::vector<TensorView*> compute_to;
    for (auto cached_output_pair : cached_outputs) {
      auto cached_output = cached_output_pair.first;
      auto output = cached_output_pair.second;

      // If an output has multiple consumers don't process here, we want only
      // terminating outputs
      if (cached_output->uses().size() > 1) {
        continue;
      }

      auto pos_it = std::find_if(
          output->domain()->domain().begin(),
          output->domain()->domain().end(),
          [&mapped_to_trivial_reduction](IterDomain* id) {
            return id->getParallelType() == ParallelType::Unswitch ||
                id->getParallelType() == ParallelType::Unroll ||
                id->getParallelType() == ParallelType::Vectorize ||
                id->getParallelType() == ParallelType::MisalignedVectorize ||
                mapped_to_trivial_reduction.count(id);
          });
      auto pos = pos_it == output->domain()->domain().end()
          ? -1
          : std::distance(output->domain()->domain().begin(), pos_it) + 1;

      cached_output->computeAt(output, pos, ComputeAtMode::BestEffort);

      compute_to.push_back(cached_output);
      if (vectorize) {
        if (std::find(
                vecotrizable_inputs_outputs.begin(),
                vecotrizable_inputs_outputs.end(),
                output) != vecotrizable_inputs_outputs.end()) {
          keep_unrolled.emplace(output);
        }
      } else {
        keep_unrolled.emplace(output);
      }
    }

    // Before compute at-ing the internal structure, remove vectorization
    // anywhere it doesn't belong. Otherwise it will mess up our inlining. Clear
    // explicit unroll or vectorization when not for input or output GMEM
    // transfers.
    for (auto tv : ir_utils::allTvs(fusion)) {
      if (!keep_unrolled.count(tv)) {
        for (const auto i : c10::irange(tv->nDims())) {
          auto id = tv->axis((int)i);
          if (id->getParallelType() == ParallelType::Unroll ||
              id->getParallelType() == ParallelType::Vectorize ||
              id->getParallelType() == ParallelType::MisalignedVectorize) {
            tv->axis((int)i)->parallelize(ParallelType::Serial);
          }
        }
      }
    }

    // Make sure not to completely inline if there's trivial reductions in the
    // fusion
    auto pos_it = std::find_if(
        reference_tv->domain()->domain().begin(),
        reference_tv->domain()->domain().end(),
        [&mapped_to_trivial_reduction](IterDomain* id) {
          return mapped_to_trivial_reduction.count(id);
        });

    auto pos = pos_it == reference_tv->domain()->domain().end()
        ? -1
        : std::distance(reference_tv->domain()->domain().begin(), pos_it) + 1;

    // Compute at inputs to rfactor dimensions
    scheduler_utils::computeAtBetween(
        compute_from, rfactor_tvs, pos, ComputeAtMode::MostInlined);

    // Inline rfactor into reduction
    if (reference_tv != reduction_tv) {
      // Compute at rfactor into following reduction, keep outside first
      // reduction iter domain in the rfactor tensor view
      for (const auto i : c10::irange(rfactor_tvs.size())) {
        if (rparams.unroll_iter_dom) {
          auto rfactor_tv = rfactor_tvs[i];
          auto rfactor_tv_dom = rfactor_tv->domain()->domain();
          auto reduction_it = std::find_if(
              rfactor_tv_dom.begin(), rfactor_tv_dom.end(), [](IterDomain* id) {
                return id->isReduction();
              });
          TORCH_INTERNAL_ASSERT(
              reduction_it != rfactor_tv_dom.end(),
              "Expected reduction axis in ",
              rfactor_tv);
          auto pos = std::distance(rfactor_tv_dom.begin(), reduction_it);
          // I would like computeAtMode here to be Standard. However, the
          // processing of welford rfactors in compute at ends up propating
          // compute at from reduction_tv->rfactor_tv to all outputs.
          rfactor_tv->computeWith(
              reduction_tvs[i], pos, ComputeAtMode::BestEffort);
        } else {
          rfactor_tvs[i]->computeWith(
              reduction_tvs[i], -1, ComputeAtMode::BestEffort);
        }
      }
    }

    // Remove anything before a reduction from compute_from
    {
      auto producers_of_reductions = DependencyCheck::getAllValsBetween(
          {fusion->inputs().begin(), fusion->inputs().end()},
          {reduction_tvs.begin(), reduction_tvs.end()});

      auto producer_tvs_of_reductions =
          ir_utils::filterByType<TensorView>(producers_of_reductions);
      compute_from.erase(
          std::remove_if(
              compute_from.begin(),
              compute_from.end(),
              [&producer_tvs_of_reductions](TensorView* compute_from_tv) {
                return std::find(
                           producer_tvs_of_reductions.begin(),
                           producer_tvs_of_reductions.end(),
                           compute_from_tv) != producer_tvs_of_reductions.end();
              }),
          compute_from.end());
    }

    // Add reduction tensor views to compute from
    compute_from.insert(
        compute_from.end(), reduction_tvs.begin(), reduction_tvs.end());

    // Compute between reductions and output caches
    scheduler_utils::computeAtBetween(
        compute_from,
        compute_to,
        -1,
        ComputeAtMode::BestEffort,
        mapped_to_trivial_reduction);

  } else {
    // Want to inline, especially backwards based on reduction_tv, otherwise
    // rfactor tv may not be inlined correctly
    auto ref_tvs = rfactor_tvs.size() ? rfactor_tvs : reduction_tvs;
    for (auto red_tv : ref_tvs) {
      auto pos_it = std::find_if(
          red_tv->domain()->domain().begin(),
          red_tv->domain()->domain().end(),
          [&mapped_to_trivial_reduction](IterDomain* id) {
            return id->getParallelType() == ParallelType::Unswitch ||
                id->getParallelType() == ParallelType::Unroll ||
                id->getParallelType() == ParallelType::Vectorize ||
                id->getParallelType() == ParallelType::MisalignedVectorize ||
                mapped_to_trivial_reduction.count(id);
          });
      auto pos = pos_it == red_tv->domain()->domain().end()
          ? -1
          : std::distance(red_tv->domain()->domain().begin(), pos_it) + 1;

      scheduler_utils::computeAtInputs(red_tv, pos, ComputeAtMode::MostInlined);
      scheduler_utils::computeWithOutputs(
          red_tv, pos, ComputeAtMode::BestEffort);
    }
  }
}

namespace {
struct id_lt {
  // Return if id0 should be before id1
  inline bool operator()(const IterDomain* id0, const IterDomain* id1) {
    // Trivial reductions should always be inner most location
    if (id0->isReduction() && id0->getParallelType() == ParallelType::Serial &&
        id0->extent()->isOneInt()) {
      return false;
    } else if (
        id1->isReduction() && id1->getParallelType() == ParallelType::Serial &&
        id1->extent()->isOneInt()) {
      return true;
    }

    // Broadcast should also be in the inner most position
    if (id0->isBroadcast() || id0->isImplicitBroadcast()) {
      return false;
    } else if (id1->isBroadcast() || id1->isImplicitBroadcast()) {
      return true;
    }

    // Non constant dimensions should be outside constant ones
    if (!id0->extent()->isConstScalar() && !id0->isThread() &&
        !id1->extent()->isConstScalar() && !id1->isThread()) {
      // Prefer pushing reductions right
      if (id0->isReduction() && !id1->isReduction()) {
        return false;
      } else {
        return true;
      }
    } else if (!id0->extent()->isConstScalar() && !id0->isThread()) {
      return true;
    } else if (!id1->extent()->isConstScalar() && !id1->isThread()) {
      return false;
    }

    // Iteration domains before reductions
    if (id0->isReduction() && !id1->isReduction()) {
      return false;
    } else if (!id0->isReduction() && id1->isReduction()) {
      return true;
    }

    // If iteration domains, block and thread before others, if reductions push
    // to the right to get out of the inliners way.
    if (id0->isBlockDim()) {
      return true;
    } else if (id1->isBlockDim()) {
      return false;
    }
    if (id0->isThreadDim()) {
      return true;
    } else if (id1->isThreadDim()) {
      return false;
    }

    // Unroll and vectorizations should be pushed right (not inside broadcast or
    // trivial reductions)
    if (id0->getParallelType() == ParallelType::Unroll ||
        id0->getParallelType() == ParallelType::Vectorize ||
        id0->getParallelType() == ParallelType::MisalignedVectorize) {
      return false;
    } else if (
        id1->getParallelType() == ParallelType::Unroll ||
        id1->getParallelType() == ParallelType::Vectorize ||
        id1->getParallelType() == ParallelType::MisalignedVectorize) {
      return true;
    }

    // Unswitch should be outside unrolled and vectorized loops
    if (id0->getParallelType() == ParallelType::Unswitch) {
      return false;
    } else if (id1->getParallelType() == ParallelType::Unswitch) {
      return true;
    }

    //[block, thread, ... unroll/vec, bcast/trivial reduce]
    if (id0->extent()->isConstScalar()) {
      return false;
    } else if (id1->extent()->isConstScalar()) {
      return true;
    }

    TORCH_INTERNAL_ASSERT(
        id0->getIterType() != IterType::Gather &&
            id1->getIterType() != IterType::Gather,
        "Gather not supported in this function.");

    TORCH_INTERNAL_ASSERT(
        false, "Error sorting out iteration domains: ", id0, " and ", id1);
  }
};
} // namespace

TensorView* sortAndRFactor(TensorView* reference_tv) {
  auto domain = reference_tv->domain()->domain();
  std::sort(domain.begin(), domain.end(), id_lt());
  std::unordered_map<int, int> reorder_map;
  std::unordered_map<IterDomain*, int> domain_pos;
  for (int axis_i = 0; axis_i < (int)domain.size(); axis_i++) {
    domain_pos[domain[axis_i]] = axis_i;
  }
  for (int old_i = 0; old_i < (int)reference_tv->nDims(); old_i++) {
    auto new_i_it = domain_pos.find(reference_tv->axis(old_i));
    TORCH_INTERNAL_ASSERT(
        new_i_it != domain_pos.end(),
        "Error in schedule reorder, didn't reorder all axes in provided tv.");
    auto new_i = new_i_it->second;
    reorder_map[old_i] = new_i;
  }
  reference_tv->reorder(reorder_map);

  std::vector<int> rfactor_axes;
  std::vector<int> rfactor_axes_no_unswitch;
  size_t reduction_dims = 0;
  for (int axis_i = 0; axis_i < (int)reference_tv->nDims(); axis_i++) {
    auto id = reference_tv->axis(axis_i);
    if (!id->isReduction()) {
      continue;
    }

    reduction_dims++;
    if (id->isThread()) {
      continue;
    }

    // Don't rfactor trivial reductions
    if (!id->isParallelized() && id->extent()->isOneInt()) {
      continue;
    }

    // We always want an rfactor axis because our inlining logic expects it. If
    // there's no parallelization to split out, just rfactor everything but the
    // unswitch dim.
    if (!(id->getParallelType() == ParallelType::Unswitch &&
          id->extent()->isOneInt())) {
      rfactor_axes_no_unswitch.emplace_back(axis_i);
    }
    rfactor_axes.emplace_back(axis_i);
  }

  if (reduction_dims == rfactor_axes.size()) {
    return ir_utils::rfactorHelper(reference_tv, rfactor_axes_no_unswitch);
  }

  return ir_utils::rfactorHelper(reference_tv, rfactor_axes);
}

} // namespace reduction_scheduler_utils
} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch