#pragma once
#include <third_party/nvfuser/scheduler/normalization.h>
#include <third_party/nvfuser/scheduler/pointwise.h>
#include <third_party/nvfuser/scheduler/reduction.h>
#include <third_party/nvfuser/scheduler/transpose.h>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

enum class TORCH_CUDA_CU_API ScheduleHeuristic {
  None,
  PointWise,
  Reduction,
  Persistent,
  Transpose
};
}
} // namespace fuser
} // namespace jit
} // namespace torch
