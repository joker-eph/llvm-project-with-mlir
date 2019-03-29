//===- GreedyPatternRewriteDriver.cpp - A greedy rewriter -----------------===//
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
//
// This file implements mlir::applyPatternsGreedily.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/PatternMatch.h"
#include "llvm/ADT/DenseMap.h"
using namespace mlir;

namespace {
class WorklistRewriter;

/// This is a worklist-driven driver for the PatternMatcher, which repeatedly
/// applies the locally optimal patterns in a roughly "bottom up" way.
class GreedyPatternRewriteDriver {
public:
  explicit GreedyPatternRewriteDriver(OwningRewritePatternList &&patterns)
      : matcher(std::move(patterns)) {
    worklist.reserve(64);
  }

  void simplifyFunction(Function *currentFunction, WorklistRewriter &rewriter);

  void addToWorklist(OperationInst *op) {
    // Check to see if the worklist already contains this op.
    if (worklistMap.count(op))
      return;

    worklistMap[op] = worklist.size();
    worklist.push_back(op);
  }

  OperationInst *popFromWorklist() {
    auto *op = worklist.back();
    worklist.pop_back();

    // This operation is no longer in the worklist, keep worklistMap up to date.
    if (op)
      worklistMap.erase(op);
    return op;
  }

  /// If the specified operation is in the worklist, remove it.  If not, this is
  /// a no-op.
  void removeFromWorklist(OperationInst *op) {
    auto it = worklistMap.find(op);
    if (it != worklistMap.end()) {
      assert(worklist[it->second] == op && "malformed worklist data structure");
      worklist[it->second] = nullptr;
    }
  }

private:
  /// The low-level pattern matcher.
  PatternMatcher matcher;

  /// The worklist for this transformation keeps track of the operations that
  /// need to be revisited, plus their index in the worklist.  This allows us to
  /// efficiently remove operations from the worklist when they are removed even
  /// if they aren't the root of a pattern.
  std::vector<OperationInst *> worklist;
  DenseMap<OperationInst *, unsigned> worklistMap;

  /// As part of canonicalization, we move constants to the top of the entry
  /// block of the current function and de-duplicate them.  This keeps track of
  /// constants we have done this for.
  DenseMap<std::pair<Attribute, Type>, OperationInst *> uniquedConstants;
};
}; // end anonymous namespace

/// This is a listener object that updates our worklists and other data
/// structures in response to operations being added and removed.
namespace {
class WorklistRewriter : public PatternRewriter {
public:
  WorklistRewriter(GreedyPatternRewriteDriver &driver, MLIRContext *context)
      : PatternRewriter(context), driver(driver) {}

  virtual void setInsertionPoint(OperationInst *op) = 0;

  // If an operation is about to be removed, make sure it is not in our
  // worklist anymore because we'd get dangling references to it.
  void notifyOperationRemoved(OperationInst *op) override {
    driver.removeFromWorklist(op);
  }

  // When the root of a pattern is about to be replaced, it can trigger
  // simplifications to its users - make sure to add them to the worklist
  // before the root is changed.
  void notifyRootReplaced(OperationInst *op) override {
    for (auto *result : op->getResults())
      // TODO: Add a result->getUsers() iterator.
      for (auto &user : result->getUses()) {
        if (auto *op = dyn_cast<OperationInst>(user.getOwner()))
          driver.addToWorklist(op);
      }

    // TODO: Walk the operand list dropping them as we go.  If any of them
    // drop to zero uses, then add them to the worklist to allow them to be
    // deleted as dead.
  }

  GreedyPatternRewriteDriver &driver;
};

} // end anonymous namespace

void GreedyPatternRewriteDriver::simplifyFunction(Function *currentFunction,
                                                  WorklistRewriter &rewriter) {
  // These are scratch vectors used in the constant folding loop below.
  SmallVector<Attribute, 8> operandConstants, resultConstants;

  while (!worklist.empty()) {
    auto *op = popFromWorklist();

    // Nulls get added to the worklist when operations are removed, ignore them.
    if (op == nullptr)
      continue;

    // If we have a constant op, unique it into the entry block.
    if (auto constant = op->dyn_cast<ConstantOp>()) {
      // If this constant is dead, remove it, being careful to keep
      // uniquedConstants up to date.
      if (constant->use_empty()) {
        auto it =
            uniquedConstants.find({constant->getValue(), constant->getType()});
        if (it != uniquedConstants.end() && it->second == op)
          uniquedConstants.erase(it);
        constant->erase();
        continue;
      }

      // Check to see if we already have a constant with this type and value:
      auto &entry = uniquedConstants[std::make_pair(constant->getValue(),
                                                    constant->getType())];
      if (entry) {
        // If this constant is already our uniqued one, then leave it alone.
        if (entry == op)
          continue;

        // Otherwise replace this redundant constant with the uniqued one.  We
        // know this is safe because we move constants to the top of the
        // function when they are uniqued, so we know they dominate all uses.
        constant->replaceAllUsesWith(entry->getResult(0));
        constant->erase();
        continue;
      }

      // If we have no entry, then we should unique this constant as the
      // canonical version.  To ensure safe dominance, move the operation to the
      // top of the function.
      entry = op;
      auto &entryBB = currentFunction->front();
      op->moveBefore(&entryBB, entryBB.begin());
      continue;
    }

    // If the operation has no side effects, and no users, then it is trivially
    // dead - remove it.
    if (op->hasNoSideEffect() && op->use_empty()) {
      op->erase();
      continue;
    }

    // Check to see if any operands to the instruction is constant and whether
    // the operation knows how to constant fold itself.
    operandConstants.clear();
    for (auto *operand : op->getOperands()) {
      Attribute operandCst;
      if (auto *operandOp = operand->getDefiningInst()) {
        if (auto operandConstantOp = operandOp->dyn_cast<ConstantOp>())
          operandCst = operandConstantOp->getValue();
      }
      operandConstants.push_back(operandCst);
    }

    // If constant folding was successful, create the result constants, RAUW the
    // operation and remove it.
    resultConstants.clear();
    if (!op->constantFold(operandConstants, resultConstants)) {
      rewriter.setInsertionPoint(op);

      for (unsigned i = 0, e = op->getNumResults(); i != e; ++i) {
        auto *res = op->getResult(i);
        if (res->use_empty()) // ignore dead uses.
          continue;

        // If we already have a canonicalized version of this constant, just
        // reuse it.  Otherwise create a new one.
        Value *cstValue;
        auto it = uniquedConstants.find({resultConstants[i], res->getType()});
        if (it != uniquedConstants.end())
          cstValue = it->second->getResult(0);
        else
          cstValue = rewriter.create<ConstantOp>(
              op->getLoc(), resultConstants[i], res->getType());

        // Add all the users of the result to the worklist so we make sure to
        // revisit them.
        //
        // TODO: Add a result->getUsers() iterator.
        for (auto &operand : op->getResult(i)->getUses()) {
          if (auto *op = dyn_cast<OperationInst>(operand.getOwner()))
            addToWorklist(op);
        }

        res->replaceAllUsesWith(cstValue);
      }

      assert(op->hasNoSideEffect() && "Constant folded op with side effects?");
      op->erase();
      continue;
    }

    // If this is a commutative binary operation with a constant on the left
    // side move it to the right side.
    if (operandConstants.size() == 2 && operandConstants[0] &&
        !operandConstants[1] && op->isCommutative()) {
      auto *newLHS = op->getOperand(1);
      op->setOperand(1, op->getOperand(0));
      op->setOperand(0, newLHS);
    }

    // Check to see if we have any patterns that match this node.
    auto match = matcher.findMatch(op);
    if (!match.first)
      continue;

    // Make sure that any new operations are inserted at this point.
    rewriter.setInsertionPoint(op);
    // We know that any pattern that matched is RewritePattern because we
    // initialized the matcher with RewritePatterns.
    auto *rewritePattern = static_cast<RewritePattern *>(match.first);
    rewritePattern->rewrite(op, std::move(match.second), rewriter);
  }

  uniquedConstants.clear();
}

static void processMLFunction(MLFunction *fn,
                              OwningRewritePatternList &&patterns) {
  class MLFuncRewriter : public WorklistRewriter {
  public:
    MLFuncRewriter(GreedyPatternRewriteDriver &theDriver, FuncBuilder &builder)
        : WorklistRewriter(theDriver, builder.getContext()), builder(builder) {}

    // Implement the hook for creating operations, and make sure that newly
    // created ops are added to the worklist for processing.
    OperationInst *createOperation(const OperationState &state) override {
      auto *result = builder.createOperation(state);
      driver.addToWorklist(result);
      return result;
    }

    void setInsertionPoint(OperationInst *op) override {
      // Any new operations should be added before this statement.
      builder.setInsertionPoint(cast<OperationInst>(op));
    }

  private:
    FuncBuilder &builder;
  };

  GreedyPatternRewriteDriver driver(std::move(patterns));
  fn->walk([&](OperationInst *stmt) { driver.addToWorklist(stmt); });

  FuncBuilder mlBuilder(fn);
  MLFuncRewriter rewriter(driver, mlBuilder);
  driver.simplifyFunction(fn, rewriter);
}

static void processCFGFunction(CFGFunction *fn,
                               OwningRewritePatternList &&patterns) {
  class CFGFuncRewriter : public WorklistRewriter {
  public:
    CFGFuncRewriter(GreedyPatternRewriteDriver &theDriver, FuncBuilder &builder)
        : WorklistRewriter(theDriver, builder.getContext()), builder(builder) {}

    // Implement the hook for creating operations, and make sure that newly
    // created ops are added to the worklist for processing.
    OperationInst *createOperation(const OperationState &state) override {
      auto *result = builder.createOperation(state);
      driver.addToWorklist(result);
      return result;
    }

    void setInsertionPoint(OperationInst *op) override {
      // Any new operations should be added before this instruction.
      builder.setInsertionPoint(cast<OperationInst>(op));
    }

  private:
    FuncBuilder &builder;
  };

  GreedyPatternRewriteDriver driver(std::move(patterns));
  for (auto &bb : *fn)
    for (auto &op : bb)
      if (auto *opInst = dyn_cast<OperationInst>(&op))
        driver.addToWorklist(opInst);

  FuncBuilder cfgBuilder(fn);
  CFGFuncRewriter rewriter(driver, cfgBuilder);
  driver.simplifyFunction(fn, rewriter);
}

/// Rewrite the specified function by repeatedly applying the highest benefit
/// patterns in a greedy work-list driven manner.
///
void mlir::applyPatternsGreedily(Function *fn,
                                 OwningRewritePatternList &&patterns) {
  if (fn->isCFG())
    processCFGFunction(fn, std::move(patterns));
  else if (fn->isML())
    processMLFunction(fn, std::move(patterns));
}
