#include <torch/csrc/jit/codegen/cuda/arith.h>
#include <torch/csrc/jit/codegen/cuda/ops/composite.h>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

ForwardDropoutResult dropout(TensorView* x, Val* prob) {
  auto p1m = sub(new Double(1.), prob);
  auto zero_check = add(eq(p1m, new Double(0.)), p1m);
  auto scale = div(new Double(1.), zero_check);
  return dropout(x, p1m, scale);
}

ForwardDropoutResult dropout(TensorView* x, Val* prob, Val* scale) {
  TORCH_INTERNAL_ASSERT(x != nullptr, "Input is invalid.");
  TORCH_INTERNAL_ASSERT(
      prob != nullptr && prob->getDataType().has_value() &&
          prob->getDataType().value() == DataType::Double,
      "Probability is not a valid Double.");
  TORCH_INTERNAL_ASSERT(
      scale != nullptr && scale->getDataType().has_value() &&
          scale->getDataType().value() == DataType::Double,
      "Scale is not a valid Double.");

  auto rand_vals = unaryOp(UnaryOpType::RandLike, x);
  auto mask = lt(rand_vals, prob);
  auto apply_mask = mul(x, mask);
  auto y = mul(apply_mask, scale);

  return {y, mask};
}

TensorView* dropout_backward(TensorView* dy, TensorView* mask, Val* scale) {
  TORCH_INTERNAL_ASSERT(dy != nullptr, "Grad Output is invalid.");
  TORCH_INTERNAL_ASSERT(mask != nullptr, "Mask is invalid");
  TORCH_INTERNAL_ASSERT(
      scale != nullptr && scale->getDataType().has_value() &&
          scale->getDataType().value() == DataType::Double,
      "Scale is not a valid Double.");

  auto grad_mask = mul(dy, mask);
  auto dx = mul(grad_mask, scale);

  return dx;
}

Val* softplus(Val* x, Val* beta, Val* threshold) {
  TORCH_INTERNAL_ASSERT(x != nullptr, "Input is invalid.");
  TORCH_INTERNAL_ASSERT(beta != nullptr, "Beta is invalid.");
  TORCH_INTERNAL_ASSERT(
      threshold != nullptr, "Threshold is not a valid Double.");

  auto op_beta = mul(x, beta);
  auto maybe_result = div(
      unaryOp(UnaryOpType::Log1p, unaryOp(UnaryOpType::Exp, op_beta)), beta);
  auto y = where(gt(op_beta, threshold), x, maybe_result);
  return y;
}

LstmResult lstm(
    TensorView* prev_cell,
    TensorView* in_x,
    TensorView* forget_x,
    TensorView* cell_x,
    TensorView* out_x) {
  TORCH_INTERNAL_ASSERT(
      prev_cell != nullptr, "Previous cell state is invalid.");
  TORCH_INTERNAL_ASSERT(in_x != nullptr, "In-gate input is invalid");
  TORCH_INTERNAL_ASSERT(forget_x != nullptr, "Forget-gate input is invalid");
  TORCH_INTERNAL_ASSERT(cell_x != nullptr, "Cell-gate input is invalid");
  TORCH_INTERNAL_ASSERT(out_x != nullptr, "Out-gate input is invalid");

  const auto in_gate = unaryOp(UnaryOpType::Sigmoid, in_x);
  const auto forget_gate = unaryOp(UnaryOpType::Sigmoid, forget_x);
  const auto cell_gate = unaryOp(UnaryOpType::Tanh, cell_x);
  const auto out_gate = unaryOp(UnaryOpType::Sigmoid, out_x);

  const auto cell = add(mul(forget_gate, prev_cell), mul(in_gate, cell_gate));
  const auto hidden = mul(out_gate, unaryOp(UnaryOpType::Tanh, cell));

  return {cell, hidden};
}

Val* gelu_backward(Val* dy, Val* x) {
  TORCH_INTERNAL_ASSERT(dy != nullptr, "Grad Output is invalid.");
  TORCH_INTERNAL_ASSERT(x != nullptr, "Mask is invalid");

  constexpr double kAlpha = M_2_SQRTPI * M_SQRT1_2 * 0.5;
  const double kHalf = 0.5;

  auto cdf_1 = mul(x, new Double(M_SQRT1_2));
  auto cdf_2 = unaryOp(UnaryOpType::Erf, cdf_1);
  auto cdf_3 = add(cdf_2, new Double(1.));
  auto cdf_4 = mul(cdf_3, new Double(kHalf));

  auto pdf_1 = mul(x, x);
  auto pdf_2 = mul(pdf_1, new Double(-kHalf));
  auto pdf_3 = unaryOp(UnaryOpType::Exp, pdf_2);

  auto out = addcmul(cdf_4, x, pdf_3, new Double(kAlpha));
  auto dx = mul(out, dy);
  return dx;
}

} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
