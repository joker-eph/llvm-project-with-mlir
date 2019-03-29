//===- LoopUtils.cpp ---- Misc utilities for loop transformation ----------===//
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
// This file implements miscellaneous loop transformation routines.
//
//===----------------------------------------------------------------------===//

#include "mlir/Transforms/LoopUtils.h"

#include "mlir/AffineOps/AffineOps.h"
#include "mlir/Analysis/AffineAnalysis.h"
#include "mlir/Analysis/AffineStructures.h"
#include "mlir/Analysis/LoopAnalysis.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Instruction.h"
#include "mlir/StandardOps/Ops.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "LoopUtils"

using namespace mlir;

/// Returns the upper bound of an unrolled loop with lower bound 'lb' and with
/// the specified trip count, stride, and unroll factor. Returns nullptr when
/// the trip count can't be expressed as an affine expression.
AffineMap mlir::getUnrolledLoopUpperBound(ConstOpPointer<AffineForOp> forOp,
                                          unsigned unrollFactor,
                                          FuncBuilder *builder) {
  auto lbMap = forOp->getLowerBoundMap();

  // Single result lower bound map only.
  if (lbMap.getNumResults() != 1)
    return AffineMap();

  // Sometimes, the trip count cannot be expressed as an affine expression.
  auto tripCount = getTripCountExpr(forOp);
  if (!tripCount)
    return AffineMap();

  AffineExpr lb(lbMap.getResult(0));
  unsigned step = forOp->getStep();
  auto newUb = lb + (tripCount - tripCount % unrollFactor - 1) * step;

  return builder->getAffineMap(lbMap.getNumDims(), lbMap.getNumSymbols(),
                               {newUb}, {});
}

/// Returns the lower bound of the cleanup loop when unrolling a loop with lower
/// bound 'lb' and with the specified trip count, stride, and unroll factor.
/// Returns an AffinMap with nullptr storage (that evaluates to false)
/// when the trip count can't be expressed as an affine expression.
AffineMap mlir::getCleanupLoopLowerBound(ConstOpPointer<AffineForOp> forOp,
                                         unsigned unrollFactor,
                                         FuncBuilder *builder) {
  auto lbMap = forOp->getLowerBoundMap();

  // Single result lower bound map only.
  if (lbMap.getNumResults() != 1)
    return AffineMap();

  // Sometimes the trip count cannot be expressed as an affine expression.
  AffineExpr tripCount(getTripCountExpr(forOp));
  if (!tripCount)
    return AffineMap();

  AffineExpr lb(lbMap.getResult(0));
  unsigned step = forOp->getStep();
  auto newLb = lb + (tripCount - tripCount % unrollFactor) * step;
  return builder->getAffineMap(lbMap.getNumDims(), lbMap.getNumSymbols(),
                               {newLb}, {});
}

/// Promotes the loop body of a forOp to its containing block if the forOp
/// was known to have a single iteration. Returns false otherwise.
// TODO(bondhugula): extend this for arbitrary affine bounds.
bool mlir::promoteIfSingleIteration(OpPointer<AffineForOp> forOp) {
  Optional<uint64_t> tripCount = getConstantTripCount(forOp);
  if (!tripCount.hasValue() || tripCount.getValue() != 1)
    return false;

  // TODO(mlir-team): there is no builder for a max.
  if (forOp->getLowerBoundMap().getNumResults() != 1)
    return false;

  // Replaces all IV uses to its single iteration value.
  auto *iv = forOp->getInductionVar();
  Instruction *forInst = forOp->getInstruction();
  if (!iv->use_empty()) {
    if (forOp->hasConstantLowerBound()) {
      auto *mlFunc = forInst->getFunction();
      FuncBuilder topBuilder(mlFunc);
      auto constOp = topBuilder.create<ConstantIndexOp>(
          forOp->getLoc(), forOp->getConstantLowerBound());
      iv->replaceAllUsesWith(constOp);
    } else {
      const AffineBound lb = forOp->getLowerBound();
      SmallVector<Value *, 4> lbOperands(lb.operand_begin(), lb.operand_end());
      FuncBuilder builder(forInst->getBlock(), Block::iterator(forInst));
      if (lb.getMap() == builder.getDimIdentityMap()) {
        // No need of generating an affine.apply.
        iv->replaceAllUsesWith(lbOperands[0]);
      } else {
        auto affineApplyOp = builder.create<AffineApplyOp>(
            forInst->getLoc(), lb.getMap(), lbOperands);
        iv->replaceAllUsesWith(affineApplyOp);
      }
    }
  }
  // Move the loop body instructions to the loop's containing block.
  auto *block = forInst->getBlock();
  block->getInstructions().splice(Block::iterator(forInst),
                                  forOp->getBody()->getInstructions());
  forOp->erase();
  return true;
}

/// Promotes all single iteration for inst's in the Function, i.e., moves
/// their body into the containing Block.
void mlir::promoteSingleIterationLoops(Function *f) {
  // Gathers all innermost loops through a post order pruned walk.
  f->walkPostOrder<AffineForOp>(
      [](OpPointer<AffineForOp> forOp) { promoteIfSingleIteration(forOp); });
}

/// Generates a 'for' inst with the specified lower and upper bounds while
/// generating the right IV remappings for the shifted instructions. The
/// instruction blocks that go into the loop are specified in instGroupQueue
/// starting from the specified offset, and in that order; the first element of
/// the pair specifies the shift applied to that group of instructions; note
/// that the shift is multiplied by the loop step before being applied. Returns
/// nullptr if the generated loop simplifies to a single iteration one.
static OpPointer<AffineForOp>
generateLoop(AffineMap lbMap, AffineMap ubMap,
             const std::vector<std::pair<uint64_t, ArrayRef<Instruction *>>>
                 &instGroupQueue,
             unsigned offset, OpPointer<AffineForOp> srcForInst,
             FuncBuilder *b) {
  SmallVector<Value *, 4> lbOperands(srcForInst->getLowerBoundOperands());
  SmallVector<Value *, 4> ubOperands(srcForInst->getUpperBoundOperands());

  assert(lbMap.getNumInputs() == lbOperands.size());
  assert(ubMap.getNumInputs() == ubOperands.size());

  auto loopChunk =
      b->create<AffineForOp>(srcForInst->getLoc(), lbOperands, lbMap,
                             ubOperands, ubMap, srcForInst->getStep());
  loopChunk->createBody();
  auto *loopChunkIV = loopChunk->getInductionVar();
  auto *srcIV = srcForInst->getInductionVar();

  BlockAndValueMapping operandMap;

  for (auto it = instGroupQueue.begin() + offset, e = instGroupQueue.end();
       it != e; ++it) {
    uint64_t shift = it->first;
    auto insts = it->second;
    // All 'same shift' instructions get added with their operands being
    // remapped to results of cloned instructions, and their IV used remapped.
    // Generate the remapping if the shift is not zero: remappedIV = newIV -
    // shift.
    if (!srcIV->use_empty() && shift != 0) {
      FuncBuilder b(loopChunk->getBody());
      auto ivRemap = b.create<AffineApplyOp>(
          srcForInst->getLoc(),
          b.getSingleDimShiftAffineMap(
              -static_cast<int64_t>(srcForInst->getStep() * shift)),
          loopChunkIV);
      operandMap.map(srcIV, ivRemap);
    } else {
      operandMap.map(srcIV, loopChunkIV);
    }
    for (auto *inst : insts) {
      loopChunk->getBody()->push_back(inst->clone(operandMap, b->getContext()));
    }
  }
  if (promoteIfSingleIteration(loopChunk))
    return OpPointer<AffineForOp>();
  return loopChunk;
}

/// Skew the instructions in the body of a 'for' instruction with the specified
/// instruction-wise shifts. The shifts are with respect to the original
/// execution order, and are multiplied by the loop 'step' before being applied.
/// A shift of zero for each instruction will lead to no change.
// The skewing of instructions with respect to one another can be used for
// example to allow overlap of asynchronous operations (such as DMA
// communication) with computation, or just relative shifting of instructions
// for better register reuse, locality or parallelism. As such, the shifts are
// typically expected to be at most of the order of the number of instructions.
// This method should not be used as a substitute for loop distribution/fission.
// This method uses an algorithm// in time linear in the number of instructions
// in the body of the for loop - (using the 'sweep line' paradigm). This method
// asserts preservation of SSA dominance. A check for that as well as that for
// memory-based depedence preservation check rests with the users of this
// method.
UtilResult mlir::instBodySkew(OpPointer<AffineForOp> forOp,
                              ArrayRef<uint64_t> shifts,
                              bool unrollPrologueEpilogue) {
  if (forOp->getBody()->empty())
    return UtilResult::Success;

  // If the trip counts aren't constant, we would need versioning and
  // conditional guards (or context information to prevent such versioning). The
  // better way to pipeline for such loops is to first tile them and extract
  // constant trip count "full tiles" before applying this.
  auto mayBeConstTripCount = getConstantTripCount(forOp);
  if (!mayBeConstTripCount.hasValue()) {
    LLVM_DEBUG(forOp->emitNote("non-constant trip count loop not handled"));
    return UtilResult::Success;
  }
  uint64_t tripCount = mayBeConstTripCount.getValue();

  assert(isInstwiseShiftValid(forOp, shifts) &&
         "shifts will lead to an invalid transformation\n");

  int64_t step = forOp->getStep();

  unsigned numChildInsts = forOp->getBody()->getInstructions().size();

  // Do a linear time (counting) sort for the shifts.
  uint64_t maxShift = 0;
  for (unsigned i = 0; i < numChildInsts; i++) {
    maxShift = std::max(maxShift, shifts[i]);
  }
  // Such large shifts are not the typical use case.
  if (maxShift >= numChildInsts) {
    forOp->emitWarning("not shifting because shifts are unrealistically large");
    return UtilResult::Success;
  }

  // An array of instruction groups sorted by shift amount; each group has all
  // instructions with the same shift in the order in which they appear in the
  // body of the 'for' inst.
  std::vector<std::vector<Instruction *>> sortedInstGroups(maxShift + 1);
  unsigned pos = 0;
  for (auto &inst : *forOp->getBody()) {
    auto shift = shifts[pos++];
    sortedInstGroups[shift].push_back(&inst);
  }

  // Unless the shifts have a specific pattern (which actually would be the
  // common use case), prologue and epilogue are not meaningfully defined.
  // Nevertheless, if 'unrollPrologueEpilogue' is set, we will treat the first
  // loop generated as the prologue and the last as epilogue and unroll these
  // fully.
  OpPointer<AffineForOp> prologue;
  OpPointer<AffineForOp> epilogue;

  // Do a sweep over the sorted shifts while storing open groups in a
  // vector, and generating loop portions as necessary during the sweep. A block
  // of instructions is paired with its shift.
  std::vector<std::pair<uint64_t, ArrayRef<Instruction *>>> instGroupQueue;

  auto origLbMap = forOp->getLowerBoundMap();
  uint64_t lbShift = 0;
  FuncBuilder b(forOp->getInstruction());
  for (uint64_t d = 0, e = sortedInstGroups.size(); d < e; ++d) {
    // If nothing is shifted by d, continue.
    if (sortedInstGroups[d].empty())
      continue;
    if (!instGroupQueue.empty()) {
      assert(d >= 1 &&
             "Queue expected to be empty when the first block is found");
      // The interval for which the loop needs to be generated here is:
      // [lbShift, min(lbShift + tripCount, d)) and the body of the
      // loop needs to have all instructions in instQueue in that order.
      OpPointer<AffineForOp> res;
      if (lbShift + tripCount * step < d * step) {
        res = generateLoop(
            b.getShiftedAffineMap(origLbMap, lbShift),
            b.getShiftedAffineMap(origLbMap, lbShift + tripCount * step),
            instGroupQueue, 0, forOp, &b);
        // Entire loop for the queued inst groups generated, empty it.
        instGroupQueue.clear();
        lbShift += tripCount * step;
      } else {
        res = generateLoop(b.getShiftedAffineMap(origLbMap, lbShift),
                           b.getShiftedAffineMap(origLbMap, d), instGroupQueue,
                           0, forOp, &b);
        lbShift = d * step;
      }
      if (!prologue && res)
        prologue = res;
      epilogue = res;
    } else {
      // Start of first interval.
      lbShift = d * step;
    }
    // Augment the list of instructions that get into the current open interval.
    instGroupQueue.push_back({d, sortedInstGroups[d]});
  }

  // Those instructions groups left in the queue now need to be processed (FIFO)
  // and their loops completed.
  for (unsigned i = 0, e = instGroupQueue.size(); i < e; ++i) {
    uint64_t ubShift = (instGroupQueue[i].first + tripCount) * step;
    epilogue = generateLoop(b.getShiftedAffineMap(origLbMap, lbShift),
                            b.getShiftedAffineMap(origLbMap, ubShift),
                            instGroupQueue, i, forOp, &b);
    lbShift = ubShift;
    if (!prologue)
      prologue = epilogue;
  }

  // Erase the original for inst.
  forOp->erase();

  if (unrollPrologueEpilogue && prologue)
    loopUnrollFull(prologue);
  if (unrollPrologueEpilogue && !epilogue &&
      epilogue->getInstruction() != prologue->getInstruction())
    loopUnrollFull(epilogue);

  return UtilResult::Success;
}

/// Unrolls this loop completely.
bool mlir::loopUnrollFull(OpPointer<AffineForOp> forOp) {
  Optional<uint64_t> mayBeConstantTripCount = getConstantTripCount(forOp);
  if (mayBeConstantTripCount.hasValue()) {
    uint64_t tripCount = mayBeConstantTripCount.getValue();
    if (tripCount == 1) {
      return promoteIfSingleIteration(forOp);
    }
    return loopUnrollByFactor(forOp, tripCount);
  }
  return false;
}

/// Unrolls and jams this loop by the specified factor or by the trip count (if
/// constant) whichever is lower.
bool mlir::loopUnrollUpToFactor(OpPointer<AffineForOp> forOp,
                                uint64_t unrollFactor) {
  Optional<uint64_t> mayBeConstantTripCount = getConstantTripCount(forOp);

  if (mayBeConstantTripCount.hasValue() &&
      mayBeConstantTripCount.getValue() < unrollFactor)
    return loopUnrollByFactor(forOp, mayBeConstantTripCount.getValue());
  return loopUnrollByFactor(forOp, unrollFactor);
}

/// Unrolls this loop by the specified factor. Returns true if the loop
/// is successfully unrolled.
bool mlir::loopUnrollByFactor(OpPointer<AffineForOp> forOp,
                              uint64_t unrollFactor) {
  assert(unrollFactor >= 1 && "unroll factor should be >= 1");

  if (unrollFactor == 1)
    return promoteIfSingleIteration(forOp);

  if (forOp->getBody()->empty())
    return false;

  auto lbMap = forOp->getLowerBoundMap();
  auto ubMap = forOp->getUpperBoundMap();

  // Loops with max/min expressions won't be unrolled here (the output can't be
  // expressed as a Function in the general case). However, the right way to
  // do such unrolling for a Function would be to specialize the loop for the
  // 'hotspot' case and unroll that hotspot.
  if (lbMap.getNumResults() != 1 || ubMap.getNumResults() != 1)
    return false;

  // Same operand list for lower and upper bound for now.
  // TODO(bondhugula): handle bounds with different operand lists.
  if (!forOp->matchingBoundOperandList())
    return false;

  Optional<uint64_t> mayBeConstantTripCount = getConstantTripCount(forOp);

  // If the trip count is lower than the unroll factor, no unrolled body.
  // TODO(bondhugula): option to specify cleanup loop unrolling.
  if (mayBeConstantTripCount.hasValue() &&
      mayBeConstantTripCount.getValue() < unrollFactor)
    return false;

  // Generate the cleanup loop if trip count isn't a multiple of unrollFactor.
  Instruction *forInst = forOp->getInstruction();
  if (getLargestDivisorOfTripCount(forOp) % unrollFactor != 0) {
    FuncBuilder builder(forInst->getBlock(), ++Block::iterator(forInst));
    auto cleanupForInst = builder.clone(*forInst)->cast<AffineForOp>();
    auto clLbMap = getCleanupLoopLowerBound(forOp, unrollFactor, &builder);
    assert(clLbMap &&
           "cleanup loop lower bound map for single result bound maps can "
           "always be determined");
    cleanupForInst->setLowerBoundMap(clLbMap);
    // Promote the loop body up if this has turned into a single iteration loop.
    promoteIfSingleIteration(cleanupForInst);

    // Adjust upper bound.
    auto unrolledUbMap =
        getUnrolledLoopUpperBound(forOp, unrollFactor, &builder);
    assert(unrolledUbMap &&
           "upper bound map can alwayys be determined for an unrolled loop "
           "with single result bounds");
    forOp->setUpperBoundMap(unrolledUbMap);
  }

  // Scale the step of loop being unrolled by unroll factor.
  int64_t step = forOp->getStep();
  forOp->setStep(step * unrollFactor);

  // Builder to insert unrolled bodies right after the last instruction in the
  // body of 'forOp'.
  FuncBuilder builder(forOp->getBody(), forOp->getBody()->end());

  // Keep a pointer to the last instruction in the original block so that we
  // know what to clone (since we are doing this in-place).
  Block::iterator srcBlockEnd = std::prev(forOp->getBody()->end());

  // Unroll the contents of 'forOp' (append unrollFactor-1 additional copies).
  auto *forOpIV = forOp->getInductionVar();
  for (unsigned i = 1; i < unrollFactor; i++) {
    BlockAndValueMapping operandMap;

    // If the induction variable is used, create a remapping to the value for
    // this unrolled instance.
    if (!forOpIV->use_empty()) {
      // iv' = iv + 1/2/3...unrollFactor-1;
      auto d0 = builder.getAffineDimExpr(0);
      auto bumpMap = builder.getAffineMap(1, 0, {d0 + i * step}, {});
      auto ivUnroll =
          builder.create<AffineApplyOp>(forOp->getLoc(), bumpMap, forOpIV);
      operandMap.map(forOpIV, ivUnroll);
    }

    // Clone the original body of 'forOp'.
    for (auto it = forOp->getBody()->begin(); it != std::next(srcBlockEnd);
         it++) {
      builder.clone(*it, operandMap);
    }
  }

  // Promote the loop body up if this has turned into a single iteration loop.
  promoteIfSingleIteration(forOp);

  return true;
}

/// Performs loop interchange on 'forOpA' and 'forOpB', where 'forOpB' is
/// nested within 'forOpA' as the only instruction in its block.
void mlir::interchangeLoops(OpPointer<AffineForOp> forOpA,
                            OpPointer<AffineForOp> forOpB) {
  auto *forOpAInst = forOpA->getInstruction();
  // 1) Slice forOpA's instruction list (which is just forOpB) just before
  // forOpA (in forOpA's parent's block) this should leave 'forOpA's
  // instruction list empty (because its perfectly nested).
  assert(&*forOpA->getBody()->begin() == forOpB->getInstruction());
  forOpAInst->getBlock()->getInstructions().splice(
      Block::iterator(forOpAInst), forOpA->getBody()->getInstructions());
  // 2) Slice forOpB's instruction list into forOpA's instruction list (this
  // leaves forOpB's instruction list empty).
  forOpA->getBody()->getInstructions().splice(
      forOpA->getBody()->begin(), forOpB->getBody()->getInstructions());
  // 3) Slice forOpA into forOpB's instruction list.
  forOpB->getBody()->getInstructions().splice(
      forOpB->getBody()->begin(), forOpAInst->getBlock()->getInstructions(),
      Block::iterator(forOpAInst));
}

/// Performs a series of loop interchanges to sink 'forOp' 'loopDepth' levels
/// deeper in the loop nest.
void mlir::sinkLoop(OpPointer<AffineForOp> forOp, unsigned loopDepth) {
  for (unsigned i = 0; i < loopDepth; ++i) {
    assert(forOp->getBody()->front().isa<AffineForOp>());
    OpPointer<AffineForOp> nextForOp =
        forOp->getBody()->front().cast<AffineForOp>();
    interchangeLoops(forOp, nextForOp);
  }
}

// Factors out common behavior to add max(`iv`, ...), min(`iv` + `offset`, ...)
// to loop bounds.
static void augmentMapAndBounds(FuncBuilder *b, Value *iv, AffineMap *map,
                                SmallVector<Value *, 4> *operands,
                                int64_t offset = 0) {
  auto bounds = llvm::to_vector<4>(map->getResults());
  operands->push_back(iv);
  auto numOperands = operands->size();
  bounds.push_back(b->getAffineDimExpr(numOperands - 1) + offset);
  *map = b->getAffineMap(numOperands, map->getNumSymbols(), bounds, {});
  canonicalizeMapAndOperands(map, operands);
}

// Stripmines `forOp` by `factor` and sinks it under each of the `targets`.
// Stripmine-sink is a primitive building block for generalized tiling of
// imperfectly nested loops.
// This transformation is purely mechanical and does not check legality,
// profitability or even structural correctness. It is the user's
// responsibility to specify `targets` that are dominated by `forOp`.
// Returns the new AffineForOps, one per `targets`, nested immediately under
// each of the `targets`.
static SmallVector<OpPointer<AffineForOp>, 8>
stripmineSink(OpPointer<AffineForOp> forOp, uint64_t factor,
              ArrayRef<OpPointer<AffineForOp>> targets) {
  // TODO(ntv): Use cheap structural assertions that targets are nested under
  // forOp and that targets are not nested under each other when DominanceInfo
  // exposes the capability. It seems overkill to construct a whole function
  // dominance tree at this point.
  auto originalStep = forOp->getStep();
  auto scaledStep = originalStep * factor;
  forOp->setStep(scaledStep);

  auto *forInst = forOp->getInstruction();
  FuncBuilder b(forInst->getBlock(), ++Block::iterator(forInst));

  // Lower-bound map creation.
  auto lbMap = forOp->getLowerBoundMap();
  SmallVector<Value *, 4> lbOperands(forOp->getLowerBoundOperands());
  augmentMapAndBounds(&b, forOp->getInductionVar(), &lbMap, &lbOperands);

  // Upper-bound map creation.
  auto ubMap = forOp->getLowerBoundMap();
  SmallVector<Value *, 4> ubOperands(forOp->getUpperBoundOperands());
  augmentMapAndBounds(&b, forOp->getInductionVar(), &ubMap, &ubOperands,
                      /*offset=*/scaledStep);

  SmallVector<OpPointer<AffineForOp>, 8> innerLoops;
  for (auto t : targets) {
    // Insert forOp just before the first instruction in the body.
    auto *body = t->getBody();
    auto &inst = body->getInstructions().front();
    FuncBuilder b(&inst);
    auto newLoop = b.create<AffineForOp>(t->getLoc(), lbOperands, lbMap,
                                         ubOperands, ubMap, originalStep);
    newLoop->createBody()->getInstructions().splice(
        newLoop->getBody()->end(), body->getInstructions(), ++body->begin(),
        body->end());
    innerLoops.push_back(newLoop);
  }

  return innerLoops;
}

// Stripmines a `forOp` by `factor` and sinks it under a single `target`.
// Returns the new AffineForOps, nested immediately under `target`.
OpPointer<AffineForOp> stripmineSink(OpPointer<AffineForOp> forOp,
                                     uint64_t factor,
                                     OpPointer<AffineForOp> target) {
  auto res =
      stripmineSink(forOp, factor, ArrayRef<OpPointer<AffineForOp>>{target});
  assert(res.size() == 1 && "Expected 1 inner forOp");
  return res[0];
}

SmallVector<SmallVector<OpPointer<AffineForOp>, 8>, 8>
mlir::tile(ArrayRef<OpPointer<AffineForOp>> forOps, ArrayRef<uint64_t> sizes,
           ArrayRef<OpPointer<AffineForOp>> targets) {
  SmallVector<SmallVector<OpPointer<AffineForOp>, 8>, 8> res;
  SmallVector<OpPointer<AffineForOp>, 8> currentTargets(targets.begin(),
                                                        targets.end());
  for (auto it : llvm::zip(forOps, sizes)) {
    auto step = stripmineSink(std::get<0>(it), std::get<1>(it), currentTargets);
    res.push_back(step);
    currentTargets = step;
  }
  return res;
}

SmallVector<OpPointer<AffineForOp>, 8>
mlir::tile(ArrayRef<OpPointer<AffineForOp>> forOps, ArrayRef<uint64_t> sizes,
           OpPointer<AffineForOp> target) {
  return tile(forOps, sizes, ArrayRef<OpPointer<AffineForOp>>{target})[0];
}
