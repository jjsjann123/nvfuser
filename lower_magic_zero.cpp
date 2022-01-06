#include <torch/csrc/jit/codegen/cuda/lower_magic_zero.h>

#include <torch/csrc/jit/codegen/cuda/dispatch.h>
#include <torch/csrc/jit/codegen/cuda/instrumentation.h>
#include <torch/csrc/jit/codegen/cuda/kernel_ir_builder.h>
#include <torch/csrc/jit/codegen/cuda/kernel_ir_dispatch.h>
#include <torch/csrc/jit/codegen/cuda/lower2device.h>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

namespace {

class MagicZeroInserter : public kir::ExprMutator {
 public:
  static std::vector<Expr*> insert(const std::vector<Expr*>& exprs) {
    MagicZeroInserter inserter(exprs);
    return inserter.exprs_;
  }

 private:
  struct InsertionInfo {
    kir::Scope* scope = nullptr;
    kir::ForLoop* fl = nullptr;
  };

  MagicZeroInserter(const std::vector<Expr*>& exprs)
      : ir_builder(GpuLower::current()->kernel()) {
    TORCH_INTERNAL_ASSERT(exprs.size());
    kir::ExprMutator::registerInsertBefore(
        exprs.front(), ir_builder.create<kir::InitMagicZero>(), nullptr);
    kir::ExprMutator::traverseAndInsert(exprs);
  }

  void handle(kir::ForLoop* fl) final {
    if (fl->isUnrolled()) {
      if (scope_.empty()) {
        kir::ExprMutator::registerInsertAfter(
            fl, ir_builder.create<kir::UpdateMagicZero>());
      } else {
        TORCH_INTERNAL_ASSERT(
            scope_.back()->exprs().size(), "Not expecting an empty loop.");
        kir::ExprMutator::registerInsertAfter(
            fl, ir_builder.create<kir::UpdateMagicZero>(), scope_.back());
      }
    } else {
      kir::ExprMutator::handle(fl);
    }
  }

  kir::IrBuilder ir_builder;

  std::vector<InsertionInfo> insertion_list_;
};

} // namespace

std::vector<Expr*> insertMagicZero(const std::vector<Expr*>& exprs) {
  FUSER_PERF_SCOPE("GpuLower::Lower::insertMagicZero");
  // Check if magic zero was even used, if not we don't have to define it or
  // update it.
  const auto gpu_lower = GpuLower::current();
  auto kernel = gpu_lower->kernel();
  const bool has_magic_zero = std::any_of(
      kernel->irStmts().begin(),
      kernel->irStmts().end(),
      [](const std::unique_ptr<Statement>& ir_node) {
        return ir_node->isA<Val>() && isMagicZero(ir_node->as<Val>());
      });

  if (!has_magic_zero) {
    return exprs;
  }

  return MagicZeroInserter::insert(exprs);
}

bool isMagicZero(Val* val) {
  auto ns = dynamic_cast<NamedScalar*>(val);
  if (ns == nullptr) {
    return false;
  }
  return ns->dtype() == DataType::Int &&
      ns->name() == std::string(kMagicZeroName);
}

bool isProtectedWithMagicZero(Val* val) {
  auto def = dynamic_cast<BinaryOp*>(val->definition());
  return def && def->getBinaryOpType() == BinaryOpType::Add &&
      isMagicZero(def->rhs());
}

} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
