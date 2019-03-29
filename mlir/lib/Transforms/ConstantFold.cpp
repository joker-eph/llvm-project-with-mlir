//===- ConstantFold.cpp - Pass that does constant folding -----------------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "mlir/IR/Builders.h"
#include "mlir/IR/CFGFunction.h"
#include "mlir/IR/StandardOps.h"
#include "mlir/IR/StmtVisitor.h"
#include "mlir/Transforms/Pass.h"
#include "mlir/Transforms/Passes.h"
using namespace mlir;

namespace {
/// Simple constant folding pass.
struct ConstantFold : public FunctionPass, StmtWalker<ConstantFold> {
  // All constants in the function post folding.
  SmallVector<SSAValue *, 8> existingConstants;
  // Operation statements that were folded and that need to be erased.
  std::vector<OperationStmt *> opStmtsToErase;
  typedef std::function<SSAValue *(Attribute *, Type *)> ConstantFactoryType;

  bool foldOperation(Operation *op,
                     SmallVectorImpl<SSAValue *> &existingConstants,
                     ConstantFactoryType constantFactory);
  void visitOperationStmt(OperationStmt *stmt);
  void visitForStmt(ForStmt *stmt);
  PassResult runOnCFGFunction(CFGFunction *f) override;
  PassResult runOnMLFunction(MLFunction *f) override;
};
} // end anonymous namespace

/// Attempt to fold the specified operation, updating the IR to match.  If
/// constants are found, we keep track of them in the existingConstants list.
///
/// This returns false if the operation was successfully folded.
bool ConstantFold::foldOperation(Operation *op,
                                 SmallVectorImpl<SSAValue *> &existingConstants,
                                 ConstantFactoryType constantFactory) {

  // If this operation is already a constant, just remember it for cleanup
  // later, and don't try to fold it.
  if (op->is<ConstantOp>()) {
    existingConstants.push_back(op->getResult(0));
    return true;
  }

  // Check to see if each of the operands is a trivial constant.  If so, get
  // the value.  If not, ignore the instruction.
  SmallVector<Attribute *, 8> operandConstants;
  for (auto *operand : op->getOperands()) {
    Attribute *operandCst = nullptr;
    if (auto *operandOp = operand->getDefiningOperation()) {
      if (auto operandConstantOp = operandOp->getAs<ConstantOp>())
        operandCst = operandConstantOp->getValue();
    }
    operandConstants.push_back(operandCst);
  }

  // Attempt to constant fold the operation.
  SmallVector<Attribute *, 8> resultConstants;
  if (op->constantFold(operandConstants, resultConstants))
    return true;

  // Ok, if everything succeeded, then we can create constants corresponding
  // to the result of the call.
  // TODO: We can try to reuse existing constants if we see them laying
  // around.
  assert(resultConstants.size() == op->getNumResults() &&
         "constant folding produced the wrong number of results");

  for (unsigned i = 0, e = op->getNumResults(); i != e; ++i) {
    auto *res = op->getResult(i);
    if (res->use_empty()) // ignore dead uses.
      continue;

    auto *cst = constantFactory(resultConstants[i], res->getType());
    existingConstants.push_back(cst);
    res->replaceAllUsesWith(cst);
  }

  return false;
}

// For now, we do a simple top-down pass over a function folding constants.  We
// don't handle conditional control flow, constant PHI nodes, folding
// conditional branches, or anything else fancy.
PassResult ConstantFold::runOnCFGFunction(CFGFunction *f) {
  existingConstants.clear();
  CFGFuncBuilder builder(f);

  for (auto &bb : *f) {
    for (auto instIt = bb.begin(), e = bb.end(); instIt != e;) {
      auto &inst = *instIt++;

      auto constantFactory = [&](Attribute *value, Type *type) -> SSAValue * {
        builder.setInsertionPoint(&inst);
        return builder.create<ConstantOp>(inst.getLoc(), value, type)
            ->getResult();
      };

      if (!foldOperation(&inst, existingConstants, constantFactory)) {
        // At this point the operation is dead, remove it.
        // TODO: This is assuming that all constant foldable operations have no
        // side effects.  When we have side effect modeling, we should verify
        // that the operation is effect-free before we remove it.  Until then
        // this is close enough.
        inst.eraseFromBlock();
      }
    }
  }

  // By the time we are done, we may have simplified a bunch of code, leaving
  // around dead constants.  Check for them now and remove them.
  for (auto *cst : existingConstants) {
    if (cst->use_empty())
      cst->getDefiningInst()->eraseFromBlock();
  }

  return success();
}

// Override the walker's operation statement visit for constant folding.
void ConstantFold::visitOperationStmt(OperationStmt *stmt) {
  auto constantFactory = [&](Attribute *value, Type *type) -> SSAValue * {
    MLFuncBuilder builder(stmt);
    return builder.create<ConstantOp>(stmt->getLoc(), value, type)->getResult();
  };
  if (!ConstantFold::foldOperation(stmt, existingConstants, constantFactory)) {
    opStmtsToErase.push_back(stmt);
  }
}

// Override the walker's for statement visit for constant folding.
void ConstantFold::visitForStmt(ForStmt *stmt) {
  stmt->constantFoldBound(/*lower=*/true);
  stmt->constantFoldBound(/*lower=*/false);
}

PassResult ConstantFold::runOnMLFunction(MLFunction *f) {
  existingConstants.clear();
  opStmtsToErase.clear();

  walk(f);
  // At this point, these operations are dead, remove them.
  // TODO: This is assuming that all constant foldable operations have no
  // side effects.  When we have side effect modeling, we should verify that
  // the operation is effect-free before we remove it.  Until then this is
  // close enough.
  for (auto *stmt : opStmtsToErase) {
    stmt->eraseFromBlock();
  }

  // By the time we are done, we may have simplified a bunch of code, leaving
  // around dead constants.  Check for them now and remove them.
  for (auto *cst : existingConstants) {
    if (cst->use_empty())
      cst->getDefiningStmt()->eraseFromBlock();
  }

  return success();
}

/// Creates a constant folding pass.
FunctionPass *mlir::createConstantFoldPass() { return new ConstantFold(); }
