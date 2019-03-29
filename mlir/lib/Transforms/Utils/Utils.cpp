//===- Utils.cpp ---- Misc utilities for code and data transformation -----===//
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
// This file implements miscellaneous transformation routines for non-loop IR
// structures.
//
//===----------------------------------------------------------------------===//

#include "mlir/Transforms/Utils.h"

#include "mlir/Analysis/AffineAnalysis.h"
#include "mlir/Analysis/AffineStructures.h"
#include "mlir/IR/Builders.h"
#include "mlir/StandardOps/StandardOps.h"
#include "mlir/Support/MathExtras.h"
#include "llvm/ADT/DenseMap.h"

using namespace mlir;

/// Return true if this operation dereferences one or more memref's.
// Temporary utility: will be replaced when this is modeled through
// side-effects/op traits. TODO(b/117228571)
static bool isMemRefDereferencingOp(const Operation &op) {
  if (op.isa<LoadOp>() || op.isa<StoreOp>() || op.isa<DmaStartOp>() ||
      op.isa<DmaWaitOp>())
    return true;
  return false;
}

/// Replaces all uses of oldMemRef with newMemRef while optionally remapping
/// old memref's indices to the new memref using the supplied affine map
/// and adding any additional indices. The new memref could be of a different
/// shape or rank, but of the same elemental type. Additional indices are added
/// at the start for now.
// TODO(mlir-team): extend this for SSAValue / CFGFunctions. Can also be easily
// extended to add additional indices at any position.
bool mlir::replaceAllMemRefUsesWith(const MLValue *oldMemRef,
                                    MLValue *newMemRef,
                                    ArrayRef<MLValue *> extraIndices,
                                    AffineMap indexRemap) {
  unsigned newMemRefRank = newMemRef->getType().cast<MemRefType>().getRank();
  (void)newMemRefRank; // unused in opt mode
  unsigned oldMemRefRank = oldMemRef->getType().cast<MemRefType>().getRank();
  (void)newMemRefRank;
  if (indexRemap) {
    assert(indexRemap.getNumInputs() == oldMemRefRank);
    assert(indexRemap.getNumResults() + extraIndices.size() == newMemRefRank);
  } else {
    assert(oldMemRefRank + extraIndices.size() == newMemRefRank);
  }

  // Assert same elemental type.
  assert(oldMemRef->getType().cast<MemRefType>().getElementType() ==
         newMemRef->getType().cast<MemRefType>().getElementType());

  // Check if memref was used in a non-deferencing context.
  for (const StmtOperand &use : oldMemRef->getUses()) {
    auto *opStmt = cast<OperationStmt>(use.getOwner());
    // Failure: memref used in a non-deferencing op (potentially escapes); no
    // replacement in these cases.
    if (!isMemRefDereferencingOp(*opStmt))
      return false;
  }

  // Walk all uses of old memref. Statement using the memref gets replaced.
  for (auto it = oldMemRef->use_begin(); it != oldMemRef->use_end();) {
    StmtOperand &use = *(it++);
    auto *opStmt = cast<OperationStmt>(use.getOwner());
    assert(isMemRefDereferencingOp(*opStmt) &&
           "memref deferencing op expected");

    auto getMemRefOperandPos = [&]() -> unsigned {
      unsigned i;
      for (i = 0; i < opStmt->getNumOperands(); i++) {
        if (opStmt->getOperand(i) == oldMemRef)
          break;
      }
      assert(i < opStmt->getNumOperands() && "operand guaranteed to be found");
      return i;
    };
    unsigned memRefOperandPos = getMemRefOperandPos();

    // Construct the new operation statement using this memref.
    OperationState state(opStmt->getContext(), opStmt->getLoc(),
                         opStmt->getName());
    state.operands.reserve(opStmt->getNumOperands() + extraIndices.size());
    // Insert the non-memref operands.
    state.operands.insert(state.operands.end(), opStmt->operand_begin(),
                          opStmt->operand_begin() + memRefOperandPos);
    state.operands.push_back(newMemRef);

    FuncBuilder builder(opStmt);
    for (auto *extraIndex : extraIndices) {
      // TODO(mlir-team): An operation/SSA value should provide a method to
      // return the position of an SSA result in its defining
      // operation.
      assert(extraIndex->getDefiningStmt()->getNumResults() == 1 &&
             "single result op's expected to generate these indices");
      assert((cast<MLValue>(extraIndex)->isValidDim() ||
              cast<MLValue>(extraIndex)->isValidSymbol()) &&
             "invalid memory op index");
      state.operands.push_back(cast<MLValue>(extraIndex));
    }

    // Construct new indices. The indices of a memref come right after it, i.e.,
    // at position memRefOperandPos + 1.
    SmallVector<SSAValue *, 4> indices(
        opStmt->operand_begin() + memRefOperandPos + 1,
        opStmt->operand_begin() + memRefOperandPos + 1 + oldMemRefRank);
    if (indexRemap) {
      auto remapOp =
          builder.create<AffineApplyOp>(opStmt->getLoc(), indexRemap, indices);
      // Remapped indices.
      for (auto *index : remapOp->getOperation()->getResults())
        state.operands.push_back(cast<MLValue>(index));
    } else {
      // No remapping specified.
      for (auto *index : indices)
        state.operands.push_back(cast<MLValue>(index));
    }

    // Insert the remaining operands unmodified.
    state.operands.insert(state.operands.end(),
                          opStmt->operand_begin() + memRefOperandPos + 1 +
                              oldMemRefRank,
                          opStmt->operand_end());

    // Result types don't change. Both memref's are of the same elemental type.
    state.types.reserve(opStmt->getNumResults());
    for (const auto *result : opStmt->getResults())
      state.types.push_back(result->getType());

    // Attributes also do not change.
    state.attributes.insert(state.attributes.end(), opStmt->getAttrs().begin(),
                            opStmt->getAttrs().end());

    // Create the new operation.
    auto *repOp = builder.createOperation(state);
    // Replace old memref's deferencing op's uses.
    unsigned r = 0;
    for (auto *res : opStmt->getResults()) {
      res->replaceAllUsesWith(repOp->getResult(r++));
    }
    opStmt->erase();
  }
  return true;
}

// Creates and inserts into 'builder' a new AffineApplyOp, with the number of
// its results equal to the number of 'operands, as a composition
// of all other AffineApplyOps reachable from input parameter 'operands'. If the
// operands were drawing results from multiple affine apply ops, this also leads
// to a collapse into a single affine apply op. The final results of the
// composed AffineApplyOp are returned in output parameter 'results'.
OperationStmt *
mlir::createComposedAffineApplyOp(FuncBuilder *builder, Location *loc,
                                  ArrayRef<MLValue *> operands,
                                  ArrayRef<OperationStmt *> affineApplyOps,
                                  SmallVectorImpl<SSAValue *> &results) {
  // Create identity map with same number of dimensions as number of operands.
  auto map = builder->getMultiDimIdentityMap(operands.size());
  // Initialize AffineValueMap with identity map.
  AffineValueMap valueMap(map, operands);

  for (auto *opStmt : affineApplyOps) {
    assert(opStmt->isa<AffineApplyOp>());
    auto affineApplyOp = opStmt->cast<AffineApplyOp>();
    // Forward substitute 'affineApplyOp' into 'valueMap'.
    valueMap.forwardSubstitute(*affineApplyOp);
  }
  // Compose affine maps from all ancestor AffineApplyOps.
  // Create new AffineApplyOp from 'valueMap'.
  unsigned numOperands = valueMap.getNumOperands();
  SmallVector<SSAValue *, 4> outOperands(numOperands);
  for (unsigned i = 0; i < numOperands; ++i) {
    outOperands[i] = valueMap.getOperand(i);
  }
  // Create new AffineApplyOp based on 'valueMap'.
  auto affineApplyOp =
      builder->create<AffineApplyOp>(loc, valueMap.getAffineMap(), outOperands);
  results.resize(operands.size());
  for (unsigned i = 0, e = operands.size(); i < e; ++i) {
    results[i] = affineApplyOp->getResult(i);
  }
  return cast<OperationStmt>(affineApplyOp->getOperation());
}

/// Given an operation statement, inserts a new single affine apply operation,
/// that is exclusively used by this operation statement, and that provides all
/// operands that are results of an affine_apply as a function of loop iterators
/// and program parameters and whose results are.
///
/// Before
///
/// for %i = 0 to #map(%N)
///   %idx = affine_apply (d0) -> (d0 mod 2) (%i)
///   "send"(%idx, %A, ...)
///   "compute"(%idx)
///
/// After
///
/// for %i = 0 to #map(%N)
///   %idx = affine_apply (d0) -> (d0 mod 2) (%i)
///   "send"(%idx, %A, ...)
///   %idx_ = affine_apply (d0) -> (d0 mod 2) (%i)
///   "compute"(%idx_)
///
/// This allows applying different transformations on send and compute (for eg.
/// different shifts/delays).
///
/// Returns nullptr either if none of opStmt's operands were the result of an
/// affine_apply and thus there was no affine computation slice to create, or if
/// all the affine_apply op's supplying operands to this opStmt do not have any
/// uses besides this opStmt. Returns the new affine_apply operation statement
/// otherwise.
OperationStmt *mlir::createAffineComputationSlice(OperationStmt *opStmt) {
  // Collect all operands that are results of affine apply ops.
  SmallVector<MLValue *, 4> subOperands;
  subOperands.reserve(opStmt->getNumOperands());
  for (auto *operand : opStmt->getOperands()) {
    auto *defStmt = operand->getDefiningStmt();
    if (defStmt && defStmt->isa<AffineApplyOp>()) {
      subOperands.push_back(operand);
    }
  }

  // Gather sequence of AffineApplyOps reachable from 'subOperands'.
  SmallVector<OperationStmt *, 4> affineApplyOps;
  getReachableAffineApplyOps(subOperands, affineApplyOps);
  // Skip transforming if there are no affine maps to compose.
  if (affineApplyOps.empty())
    return nullptr;

  // Check if all uses of the affine apply op's lie in this op stmt
  // itself, in which case there would be nothing to do.
  bool localized = true;
  for (auto *op : affineApplyOps) {
    for (auto *result : op->getResults()) {
      for (auto &use : result->getUses()) {
        if (use.getOwner() != opStmt) {
          localized = false;
          break;
        }
      }
    }
  }
  if (localized)
    return nullptr;

  FuncBuilder builder(opStmt);
  SmallVector<SSAValue *, 4> results;
  auto *affineApplyStmt = createComposedAffineApplyOp(
      &builder, opStmt->getLoc(), subOperands, affineApplyOps, results);
  assert(results.size() == subOperands.size() &&
         "number of results should be the same as the number of subOperands");

  // Construct the new operands that include the results from the composed
  // affine apply op above instead of existing ones (subOperands). So, they
  // differ from opStmt's operands only for those operands in 'subOperands', for
  // which they will be replaced by the corresponding one from 'results'.
  SmallVector<MLValue *, 4> newOperands(opStmt->getOperands());
  for (unsigned i = 0, e = newOperands.size(); i < e; i++) {
    // Replace the subOperands from among the new operands.
    unsigned j, f;
    for (j = 0, f = subOperands.size(); j < f; j++) {
      if (newOperands[i] == subOperands[j])
        break;
    }
    if (j < subOperands.size()) {
      newOperands[i] = cast<MLValue>(results[j]);
    }
  }

  for (unsigned idx = 0; idx < newOperands.size(); idx++) {
    opStmt->setOperand(idx, newOperands[idx]);
  }

  return affineApplyStmt;
}

void mlir::forwardSubstitute(OpPointer<AffineApplyOp> affineApplyOp) {
  if (affineApplyOp->getOperation()->getOperationFunction()->getKind() !=
      Function::Kind::MLFunc) {
    // TODO: Support forward substitution for CFGFunctions.
    return;
  }
  auto *opStmt = cast<OperationStmt>(affineApplyOp->getOperation());
  // Iterate through all uses of all results of 'opStmt', forward substituting
  // into any uses which are AffineApplyOps.
  for (unsigned resultIndex = 0, e = opStmt->getNumResults(); resultIndex < e;
       ++resultIndex) {
    const MLValue *result = opStmt->getResult(resultIndex);
    for (auto it = result->use_begin(); it != result->use_end();) {
      StmtOperand &use = *(it++);
      auto *useStmt = use.getOwner();
      auto *useOpStmt = dyn_cast<OperationStmt>(useStmt);
      // Skip if use is not AffineApplyOp.
      if (useOpStmt == nullptr || !useOpStmt->isa<AffineApplyOp>())
        continue;
      // Advance iterator past 'opStmt' operands which also use 'result'.
      while (it != result->use_end() && it->getOwner() == useStmt)
        ++it;

      FuncBuilder builder(useOpStmt);
      // Initialize AffineValueMap with 'affineApplyOp' which uses 'result'.
      auto oldAffineApplyOp = useOpStmt->cast<AffineApplyOp>();
      AffineValueMap valueMap(*oldAffineApplyOp);
      // Forward substitute 'result' at index 'i' into 'valueMap'.
      valueMap.forwardSubstituteSingle(*affineApplyOp, resultIndex);

      // Create new AffineApplyOp from 'valueMap'.
      unsigned numOperands = valueMap.getNumOperands();
      SmallVector<SSAValue *, 4> operands(numOperands);
      for (unsigned i = 0; i < numOperands; ++i) {
        operands[i] = valueMap.getOperand(i);
      }
      auto newAffineApplyOp = builder.create<AffineApplyOp>(
          useOpStmt->getLoc(), valueMap.getAffineMap(), operands);

      // Update all uses to use results from 'newAffineApplyOp'.
      for (unsigned i = 0, e = useOpStmt->getNumResults(); i < e; ++i) {
        oldAffineApplyOp->getResult(i)->replaceAllUsesWith(
            newAffineApplyOp->getResult(i));
      }
      // Erase 'oldAffineApplyOp'.
      oldAffineApplyOp->getOperation()->erase();
    }
  }
}

/// Folds the specified (lower or upper) bound to a constant if possible
/// considering its operands. Returns false if the folding happens for any of
/// the bounds, true otherwise.
bool mlir::constantFoldBounds(ForStmt *forStmt) {
  auto foldLowerOrUpperBound = [forStmt](bool lower) {
    // Check if the bound is already a constant.
    if (lower && forStmt->hasConstantLowerBound())
      return true;
    if (!lower && forStmt->hasConstantUpperBound())
      return true;

    // Check to see if each of the operands is the result of a constant.  If so,
    // get the value.  If not, ignore it.
    SmallVector<Attribute, 8> operandConstants;
    auto boundOperands = lower ? forStmt->getLowerBoundOperands()
                               : forStmt->getUpperBoundOperands();
    for (const auto *operand : boundOperands) {
      Attribute operandCst;
      if (auto *operandOp = operand->getDefiningOperation()) {
        if (auto operandConstantOp = operandOp->dyn_cast<ConstantOp>())
          operandCst = operandConstantOp->getValue();
      }
      operandConstants.push_back(operandCst);
    }

    AffineMap boundMap =
        lower ? forStmt->getLowerBoundMap() : forStmt->getUpperBoundMap();
    assert(boundMap.getNumResults() >= 1 &&
           "bound maps should have at least one result");
    SmallVector<Attribute, 4> foldedResults;
    if (boundMap.constantFold(operandConstants, foldedResults))
      return true;

    // Compute the max or min as applicable over the results.
    assert(!foldedResults.empty() && "bounds should have at least one result");
    auto maxOrMin = foldedResults[0].cast<IntegerAttr>().getValue();
    for (unsigned i = 1; i < foldedResults.size(); i++) {
      auto foldedResult = foldedResults[i].cast<IntegerAttr>().getValue();
      maxOrMin = lower ? std::max(maxOrMin, foldedResult)
                       : std::min(maxOrMin, foldedResult);
    }
    lower ? forStmt->setConstantLowerBound(maxOrMin)
          : forStmt->setConstantUpperBound(maxOrMin);

    // Return false on success.
    return false;
  };

  bool ret = foldLowerOrUpperBound(/*lower=*/true);
  ret &= foldLowerOrUpperBound(/*lower=*/false);
  return ret;
}
