#pragma once

#include <c10/macros/Export.h>

#include <torch/csrc/jit/codegen/cuda/dispatch.h>
#include <torch/csrc/jit/codegen/cuda/ir_all_nodes.h>
#include <torch/csrc/jit/codegen/cuda/kernel_ir.h>

#include <vector>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

//! Collects start and stop offsets of all split root domains. Offsets
//! are zero unless partially split.
class TORCH_CUDA_CU_API PartialSplitMap {
 public:
  void build(Fusion* fusion);

  Val* getStartOffset(IterDomain* root_domain) const;
  Val* kirGetStartOffset(IterDomain* root_domain) const;
  Val* getStopOffset(IterDomain* root_domain) const;
  Val* kirGetStopOffset(IterDomain* root_domain) const;

 private:
  std::unordered_map<IterDomain*, Val*> start_offset_map_;
  std::unordered_map<IterDomain*, Val*> kir_start_offset_map_;
  std::unordered_map<IterDomain*, Val*> stop_offset_map_;
  std::unordered_map<IterDomain*, Val*> kir_stop_offset_map_;
};

} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
