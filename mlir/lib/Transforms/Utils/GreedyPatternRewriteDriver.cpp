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
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/StandardOps/Ops.h"
#include "llvm/ADT/DenseMap.h"
using namespace mlir;

namespace {

/// This is a worklist-driven driver for the PatternMatcher, which repeatedly
/// applies the locally optimal patterns in a roughly "bottom up" way.
class GreedyPatternRewriteDriver : public PatternRewriter {
public:
  explicit GreedyPatternRewriteDriver(Function *fn,
                                      OwningRewritePatternList &&patterns)
      : PatternRewriter(fn->getContext()), matcher(std::move(patterns)),
        builder(fn) {
    worklist.reserve(64);

    // Add all operations to the worklist.
    fn->walk([&](Instruction *inst) { addToWorklist(inst); });
  }

  /// Perform the rewrites.
  void simplifyFunction();

  void addToWorklist(Instruction *op) {
    // Check to see if the worklist already contains this op.
    if (worklistMap.count(op))
      return;

    worklistMap[op] = worklist.size();
    worklist.push_back(op);
  }

  Instruction *popFromWorklist() {
    auto *op = worklist.back();
    worklist.pop_back();

    // This operation is no longer in the worklist, keep worklistMap up to date.
    if (op)
      worklistMap.erase(op);
    return op;
  }

  /// If the specified operation is in the worklist, remove it.  If not, this is
  /// a no-op.
  void removeFromWorklist(Instruction *op) {
    auto it = worklistMap.find(op);
    if (it != worklistMap.end()) {
      assert(worklist[it->second] == op && "malformed worklist data structure");
      worklist[it->second] = nullptr;
    }
  }

  // These are hooks implemented for PatternRewriter.
protected:
  // Implement the hook for creating operations, and make sure that newly
  // created ops are added to the worklist for processing.
  Instruction *createOperation(const OperationState &state) override {
    auto *result = builder.createOperation(state);
    addToWorklist(result);
    return result;
  }

  // If an operation is about to be removed, make sure it is not in our
  // worklist anymore because we'd get dangling references to it.
  void notifyOperationRemoved(Instruction *op) override {
    addToWorklist(op->getOperands());
    removeFromWorklist(op);
  }

  // When the root of a pattern is about to be replaced, it can trigger
  // simplifications to its users - make sure to add them to the worklist
  // before the root is changed.
  void notifyRootReplaced(Instruction *op) override {
    for (auto *result : op->getResults())
      // TODO: Add a result->getUsers() iterator.
      for (auto &user : result->getUses())
        addToWorklist(user.getOwner());
  }

private:
  // Look over the provided operands for any defining instructions that should
  // be re-added to the worklist. This function should be called when an
  // operation is modified or removed, as it may trigger further
  // simplifications.
  template <typename Operands> void addToWorklist(Operands &&operands) {
    for (Value *operand : operands) {
      // If the use count of this operand is now < 2, we re-add the defining
      // instruction to the worklist.
      // TODO(riverriddle) This is based on the fact that zero use instructions
      // may be deleted, and that single use values often have more
      // canonicalization opportunities.
      if (!operand->use_empty() &&
          std::next(operand->use_begin()) != operand->use_end())
        continue;
      if (auto *defInst = operand->getDefiningInst())
        addToWorklist(defInst);
    }
  }

  /// The low-level pattern matcher.
  PatternMatcher matcher;

  /// This builder is used to create new operations.
  FuncBuilder builder;

  /// The worklist for this transformation keeps track of the operations that
  /// need to be revisited, plus their index in the worklist.  This allows us to
  /// efficiently remove operations from the worklist when they are erased from
  /// the function, even if they aren't the root of a pattern.
  std::vector<Instruction *> worklist;
  DenseMap<Instruction *, unsigned> worklistMap;

  /// As part of canonicalization, we move constants to the top of the entry
  /// block of the current function and de-duplicate them.  This keeps track of
  /// constants we have done this for.
  DenseMap<std::pair<Attribute, Type>, Instruction *> uniquedConstants;
};
}; // end anonymous namespace

/// Perform the rewrites.
void GreedyPatternRewriteDriver::simplifyFunction() {
  // These are scratch vectors used in the constant folding loop below.
  SmallVector<Attribute, 8> operandConstants, resultConstants;
  SmallVector<Value *, 8> originalOperands, resultValues;

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
      auto &entryBB = builder.getInsertionBlock()->getFunction()->front();
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
    operandConstants.assign(op->getNumOperands(), Attribute());
    for (unsigned i = 0, e = op->getNumOperands(); i != e; ++i)
      matchPattern(op->getOperand(i), m_Constant(&operandConstants[i]));

    // If this is a commutative binary operation with a constant on the left
    // side move it to the right side.
    if (operandConstants.size() == 2 && operandConstants[0] &&
        !operandConstants[1] && op->isCommutative()) {
      std::swap(op->getInstOperand(0), op->getInstOperand(1));
      std::swap(operandConstants[0], operandConstants[1]);
    }

    // If constant folding was successful, create the result constants, RAUW the
    // operation and remove it.
    resultConstants.clear();
    if (!op->constantFold(operandConstants, resultConstants)) {
      builder.setInsertionPoint(op);

      // Add the operands to the worklist for visitation.
      addToWorklist(op->getOperands());

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
          cstValue = create<ConstantOp>(op->getLoc(), res->getType(),
                                        resultConstants[i]);

        // Add all the users of the result to the worklist so we make sure to
        // revisit them.
        //
        // TODO: Add a result->getUsers() iterator.
        for (auto &operand : op->getResult(i)->getUses())
          addToWorklist(operand.getOwner());

        res->replaceAllUsesWith(cstValue);
      }

      assert(op->hasNoSideEffect() && "Constant folded op with side effects?");
      op->erase();
      continue;
    }

    // Otherwise see if we can use the generic folder API to simplify the
    // operation.
    originalOperands.assign(op->operand_begin(), op->operand_end());
    resultValues.clear();
    if (!op->fold(resultValues)) {
      // If the result was an in-place simplification (e.g. max(x,x,y) ->
      // max(x,y)) then add the original operands to the worklist so we can make
      // sure to revisit them.
      if (resultValues.empty()) {
        // Add the operands back to the worklist as there may be more
        // canonicalization opportunities now.
        addToWorklist(originalOperands);
      } else {
        // Otherwise, the operation is simplified away completely.
        assert(resultValues.size() == op->getNumResults());

        // Notify that we are replacing this operation.
        notifyRootReplaced(op);

        // Replace the result values and erase the operation.
        for (unsigned i = 0, e = resultValues.size(); i != e; ++i) {
          auto *res = op->getResult(i);
          if (!res->use_empty())
            res->replaceAllUsesWith(resultValues[i]);
        }

        notifyOperationRemoved(op);
        op->erase();
      }
      continue;
    }

    // Check to see if we have any patterns that match this node.
    auto match = matcher.findMatch(op);
    if (!match.first)
      continue;

    // Make sure that any new operations are inserted at this point.
    builder.setInsertionPoint(op);
    // We know that any pattern that matched is RewritePattern because we
    // initialized the matcher with RewritePatterns.
    auto *rewritePattern = static_cast<RewritePattern *>(match.first);
    rewritePattern->rewrite(op, std::move(match.second), *this);
  }

  uniquedConstants.clear();
}

/// Rewrite the specified function by repeatedly applying the highest benefit
/// patterns in a greedy work-list driven manner.
///
void mlir::applyPatternsGreedily(Function *fn,
                                 OwningRewritePatternList &&patterns) {
  GreedyPatternRewriteDriver driver(fn, std::move(patterns));
  driver.simplifyFunction();
}
