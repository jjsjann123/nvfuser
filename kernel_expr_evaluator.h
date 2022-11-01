
#pragma once

#include <c10/macros/Export.h>

#include <torch/csrc/jit/codegen/cuda/dispatch.h>
#include <torch/csrc/jit/codegen/cuda/dynamic_type.h>
#include <torch/csrc/jit/codegen/cuda/evaluator_common.h>
#include <torch/csrc/jit/codegen/cuda/kernel_ir.h>

#include <c10/util/Optional.h>

#include <unordered_map>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

class GpuLower;

namespace kir {

//! Calculate Kernel IR expressions
//!
//! How to evaluate Kernel IR expressions:
//!
//! ```cpp
//!   kir::ExpressionEvaluator eval;
//!   eval.bind(symbolic_value, concrete_value);
//!   ... bind more values ...
//!   const auto result = eval.evaluate(interesting_value);
//!   if (result.has_value()) {
//!     ... we have successfully calculated the result ...
//!   } else {
//!     ... expression can't be evaluated ...
//!   }
//! ```
//!
class TORCH_CUDA_CU_API ExpressionEvaluator : private OptInConstDispatch {
 public:
  //! Set a concrete value for a symbolic value
  void bind(const Val* value, IntOrDouble concrete_value);

  //! Set a concrete value for a parallel dimension
  void bind(ParallelType pt, Int::ScalarType concrete_value);

  //! Try to evaluate a Kernel IR value
  c10::optional<IntOrDouble> evaluate(const Val* value);

  //! Debugging helper, prints all the currently known values
  void print() const;

  auto& precomputedValues() {
    return precomputed_values_;
  }

 private:
  c10::optional<IntOrDouble> getValue(const Val* value);

  void handle(const UnaryOp* unary_op) final;
  void handle(const BinaryOp* binary_op) final;

 private:
  PrecomputedValues* precomputed_values_ = nullptr;
  std::unordered_map<const Val*, IntOrDouble> known_values_;
  std::unordered_map<std::string, IntOrDouble> known_named_scalars_;
};

} // namespace kir
} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
