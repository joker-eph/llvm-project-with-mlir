//===- Utils.cpp ---- Misc utilities for analysis -------------------------===//
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
// This file implements miscellaneous analysis routines for non-loop IR
// structures.
//
//===----------------------------------------------------------------------===//

#include "mlir/Analysis/Utils.h"

#include "mlir/Analysis/AffineAnalysis.h"
#include "mlir/Analysis/AffineStructures.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/StandardOps/StandardOps.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "analysis-utils"

using namespace mlir;

/// Populates 'loops' with IVs of the loops surrounding 'inst' ordered from
/// the outermost 'for' instruction to the innermost one.
void mlir::getLoopIVs(const Instruction &inst,
                      SmallVectorImpl<ForInst *> *loops) {
  auto *currInst = inst.getParentInst();
  ForInst *currForInst;
  // Traverse up the hierarchy collecing all 'for' instruction while skipping
  // over 'if' instructions.
  while (currInst && ((currForInst = dyn_cast<ForInst>(currInst)) ||
                      isa<IfInst>(currInst))) {
    if (currForInst)
      loops->push_back(currForInst);
    currInst = currInst->getParentInst();
  }
  std::reverse(loops->begin(), loops->end());
}

unsigned MemRefRegion::getRank() const {
  return memref->getType().cast<MemRefType>().getRank();
}

Optional<int64_t> MemRefRegion::getBoundingConstantSizeAndShape(
    SmallVectorImpl<int> *shape,
    std::vector<SmallVector<int64_t, 4>> *lbs) const {
  auto memRefType = memref->getType().cast<MemRefType>();
  unsigned rank = memRefType.getRank();
  shape->reserve(rank);

  // Find a constant upper bound on the extent of this memref region along each
  // dimension.
  int64_t numElements = 1;
  int64_t diffConstant;
  for (unsigned d = 0; d < rank; d++) {
    SmallVector<int64_t, 4> lb;
    Optional<int64_t> diff = cst.getConstantBoundOnDimSize(d, &lb);
    if (diff.hasValue()) {
      diffConstant = diff.getValue();
    } else {
      // If no constant bound is found, then it can always be bound by the
      // memref's dim size if the latter has a constant size along this dim.
      auto dimSize = memRefType.getDimSize(d);
      if (dimSize == -1)
        return None;
      diffConstant = dimSize;
      // Lower bound becomes 0.
      lb.resize(cst.getNumSymbolIds() + 1, 0);
    }
    numElements *= diffConstant;
    if (lbs) {
      lbs->push_back(lb);
    }
    if (shape) {
      shape->push_back(diffConstant);
    }
  }
  return numElements;
}

/// Computes the memory region accessed by this memref with the region
/// represented as constraints symbolic/parameteric in 'loopDepth' loops
/// surrounding opInst and any additional Function symbols. Returns false if
/// this fails due to yet unimplemented cases.
//  For example, the memref region for this load operation at loopDepth = 1 will
//  be as below:
//
//    for %i = 0 to 32 {
//      for %ii = %i to (d0) -> (d0 + 8) (%i) {
//        load %A[%ii]
//      }
//    }
//
// region:  {memref = %A, write = false, {%i <= m0 <= %i + 7} }
// The last field is a 2-d FlatAffineConstraints symbolic in %i.
//
// TODO(bondhugula): extend this to any other memref dereferencing ops
// (dma_start, dma_wait).
bool mlir::getMemRefRegion(OperationInst *opInst, unsigned loopDepth,
                           MemRefRegion *region) {
  OpPointer<LoadOp> loadOp;
  OpPointer<StoreOp> storeOp;
  unsigned rank;
  SmallVector<Value *, 4> indices;

  if ((loadOp = opInst->dyn_cast<LoadOp>())) {
    rank = loadOp->getMemRefType().getRank();
    indices.append(loadOp->getIndices().begin(), loadOp->getIndices().end());
    region->memref = loadOp->getMemRef();
    region->setWrite(false);
  } else if ((storeOp = opInst->dyn_cast<StoreOp>())) {
    rank = storeOp->getMemRefType().getRank();
    indices.append(storeOp->getIndices().begin(), storeOp->getIndices().end());
    region->memref = storeOp->getMemRef();
    region->setWrite(true);
  } else {
    return false;
  }

  // Build the constraints for this region.
  FlatAffineConstraints *regionCst = region->getConstraints();

  if (rank == 0) {
    // A rank 0 memref has a 0-d region.
    SmallVector<ForInst *, 4> ivs;
    getLoopIVs(*opInst, &ivs);
    SmallVector<Value *, 4> regionSymbols(ivs.begin(), ivs.end());
    regionCst->reset(0, loopDepth, 0, regionSymbols);
    return true;
  }

  FuncBuilder b(opInst);
  auto idMap = b.getMultiDimIdentityMap(rank);
  // Initialize 'accessValueMap' and compose with reachable AffineApplyOps.
  fullyComposeAffineMapAndOperands(&idMap, &indices);
  AffineValueMap accessValueMap(idMap, indices);
  AffineMap accessMap = accessValueMap.getAffineMap();

  // We'll first associate the dims and symbols of the access map to the dims
  // and symbols resp. of regionCst. This will change below once regionCst is
  // fully constructed out.
  regionCst->reset(accessMap.getNumDims(), accessMap.getNumSymbols(), 0,
                   accessValueMap.getOperands());

  // Add equality constraints.
  unsigned numDims = accessMap.getNumDims();
  unsigned numSymbols = accessMap.getNumSymbols();
  // Add inequalties for loop lower/upper bounds.
  for (unsigned i = 0; i < numDims + numSymbols; ++i) {
    if (auto *loop = dyn_cast<ForInst>(accessValueMap.getOperand(i))) {
      // Note that regionCst can now have more dimensions than accessMap if the
      // bounds expressions involve outer loops or other symbols.
      // TODO(bondhugula): rewrite this to use getInstIndexSet; this way
      // conditionals will be handled when the latter supports it.
      if (!regionCst->addForInstDomain(*loop))
        return false;
    } else {
      // Has to be a valid symbol.
      auto *symbol = accessValueMap.getOperand(i);
      assert(symbol->isValidSymbol());
      // Check if the symbol is a constant.
      if (auto *opInst = symbol->getDefiningInst()) {
        if (auto constOp = opInst->dyn_cast<ConstantIndexOp>()) {
          regionCst->setIdToConstant(*symbol, constOp->getValue());
        }
      }
    }
  }

  // Add access function equalities to connect loop IVs to data dimensions.
  if (!regionCst->composeMap(&accessValueMap)) {
    LLVM_DEBUG(llvm::dbgs() << "getMemRefRegion: compose affine map failed\n");
    return false;
  }

  // Eliminate any loop IVs other than the outermost 'loopDepth' IVs, on which
  // this memref region is symbolic.
  SmallVector<ForInst *, 4> outerIVs;
  getLoopIVs(*opInst, &outerIVs);
  outerIVs.resize(loopDepth);
  for (auto *operand : accessValueMap.getOperands()) {
    ForInst *iv;
    if ((iv = dyn_cast<ForInst>(operand)) &&
        std::find(outerIVs.begin(), outerIVs.end(), iv) == outerIVs.end()) {
      regionCst->projectOut(operand);
    }
  }
  // Project out any local variables (these would have been added for any
  // mod/divs).
  regionCst->projectOut(regionCst->getNumDimIds() +
                            regionCst->getNumSymbolIds(),
                        regionCst->getNumLocalIds());

  // Set all identifiers appearing after the first 'rank' identifiers as
  // symbolic identifiers - so that the ones correspoding to the memref
  // dimensions are the dimensional identifiers for the memref region.
  regionCst->setDimSymbolSeparation(regionCst->getNumIds() - rank);

  // Constant fold any symbolic identifiers.
  regionCst->constantFoldIdRange(/*pos=*/regionCst->getNumDimIds(),
                                 /*num=*/regionCst->getNumSymbolIds());

  assert(regionCst->getNumDimIds() == rank && "unexpected MemRefRegion format");

  return true;
}

/// Returns the size of memref data in bytes if it's statically shaped, None
/// otherwise.  If the element of the memref has vector type, takes into account
/// size of the vector as well.
Optional<uint64_t> mlir::getMemRefSizeInBytes(MemRefType memRefType) {
  if (memRefType.getNumDynamicDims() > 0)
    return None;
  auto elementType = memRefType.getElementType();
  if (!elementType.isIntOrFloat() && !elementType.isa<VectorType>())
    return None;

  uint64_t sizeInBits;
  if (elementType.isIntOrFloat()) {
    sizeInBits = elementType.getIntOrFloatBitWidth();
  } else {
    auto vectorType = elementType.cast<VectorType>();
    sizeInBits =
        vectorType.getElementTypeBitWidth() * vectorType.getNumElements();
  }
  for (unsigned i = 0, e = memRefType.getRank(); i < e; i++) {
    sizeInBits = sizeInBits * memRefType.getDimSize(i);
  }
  return llvm::divideCeil(sizeInBits, 8);
}

template <typename LoadOrStoreOpPointer>
bool mlir::boundCheckLoadOrStoreOp(LoadOrStoreOpPointer loadOrStoreOp,
                                   bool emitError) {
  static_assert(
      std::is_same<LoadOrStoreOpPointer, OpPointer<LoadOp>>::value ||
          std::is_same<LoadOrStoreOpPointer, OpPointer<StoreOp>>::value,
      "function argument should be either a LoadOp or a StoreOp");

  OperationInst *opInst = loadOrStoreOp->getInstruction();
  MemRefRegion region;
  if (!getMemRefRegion(opInst, /*loopDepth=*/0, &region))
    return false;
  LLVM_DEBUG(llvm::dbgs() << "Memory region");
  LLVM_DEBUG(region.getConstraints()->dump());

  bool outOfBounds = false;
  unsigned rank = loadOrStoreOp->getMemRefType().getRank();

  // For each dimension, check for out of bounds.
  for (unsigned r = 0; r < rank; r++) {
    FlatAffineConstraints ucst(*region.getConstraints());

    // Intersect memory region with constraint capturing out of bounds (both out
    // of upper and out of lower), and check if the constraint system is
    // feasible. If it is, there is at least one point out of bounds.
    SmallVector<int64_t, 4> ineq(rank + 1, 0);
    int dimSize = loadOrStoreOp->getMemRefType().getDimSize(r);
    // TODO(bondhugula): handle dynamic dim sizes.
    if (dimSize == -1)
      continue;

    // Check for overflow: d_i >= memref dim size.
    ucst.addConstantLowerBound(r, dimSize);
    outOfBounds = !ucst.isEmpty();
    if (outOfBounds && emitError) {
      loadOrStoreOp->emitOpError(
          "memref out of upper bound access along dimension #" + Twine(r + 1));
    }

    // Check for a negative index.
    FlatAffineConstraints lcst(*region.getConstraints());
    std::fill(ineq.begin(), ineq.end(), 0);
    // d_i <= -1;
    lcst.addConstantUpperBound(r, -1);
    outOfBounds = !lcst.isEmpty();
    if (outOfBounds && emitError) {
      loadOrStoreOp->emitOpError(
          "memref out of lower bound access along dimension #" + Twine(r + 1));
    }
  }
  return outOfBounds;
}

// Explicitly instantiate the template so that the compiler knows we need them!
template bool mlir::boundCheckLoadOrStoreOp(OpPointer<LoadOp> loadOp,
                                            bool emitError);
template bool mlir::boundCheckLoadOrStoreOp(OpPointer<StoreOp> storeOp,
                                            bool emitError);

// Returns in 'positions' the Block positions of 'inst' in each ancestor
// Block from the Block containing instruction, stopping at 'limitBlock'.
static void findInstPosition(const Instruction *inst, Block *limitBlock,
                             SmallVectorImpl<unsigned> *positions) {
  const Block *block = inst->getBlock();
  while (block != limitBlock) {
    int instPosInBlock = block->findInstPositionInBlock(*inst);
    assert(instPosInBlock >= 0);
    positions->push_back(instPosInBlock);
    inst = block->getContainingInst();
    block = inst->getBlock();
  }
  std::reverse(positions->begin(), positions->end());
}

// Returns the Instruction in a possibly nested set of Blocks, where the
// position of the instruction is represented by 'positions', which has a
// Block position for each level of nesting.
static Instruction *getInstAtPosition(ArrayRef<unsigned> positions,
                                      unsigned level, Block *block) {
  unsigned i = 0;
  for (auto &inst : *block) {
    if (i != positions[level]) {
      ++i;
      continue;
    }
    if (level == positions.size() - 1)
      return &inst;
    if (auto *childForInst = dyn_cast<ForInst>(&inst))
      return getInstAtPosition(positions, level + 1, childForInst->getBody());

    if (auto *ifInst = dyn_cast<IfInst>(&inst)) {
      auto *ret = getInstAtPosition(positions, level + 1, ifInst->getThen());
      if (ret != nullptr)
        return ret;
      if (auto *elseClause = ifInst->getElse())
        return getInstAtPosition(positions, level + 1, elseClause);
    }
  }
  return nullptr;
}

// Computes memref dependence between 'srcAccess' and 'dstAccess' and uses the
// dependence constraint system to create AffineMaps with which to adjust the
// loop bounds of the inserted compution slice so that they are functions of the
// loop IVs and symbols of the loops surrounding 'dstAccess'.
// TODO(andydavis,bondhugula): extend the slicing utility to compute slices that
// aren't necessarily a one-to-one relation b/w the source and destination. The
// relation between the source and destination could be many-to-many in general.
// TODO(andydavis,bondhugula): the slice computation is incorrect in the cases
// where the dependence from the source to the destination does not cover the
// entire destination index set. Subtract out the dependent destination
// iterations from destination index set and check for emptiness --- this is one
// solution.
ForInst *mlir::insertBackwardComputationSlice(MemRefAccess *srcAccess,
                                              MemRefAccess *dstAccess,
                                              unsigned srcLoopDepth,
                                              unsigned dstLoopDepth) {
  FlatAffineConstraints dependenceConstraints;
  if (!checkMemrefAccessDependence(*srcAccess, *dstAccess, /*loopDepth=*/1,
                                   &dependenceConstraints,
                                   /*dependenceComponents=*/nullptr)) {
    return nullptr;
  }
  // Get loop nest surrounding src operation.
  SmallVector<ForInst *, 4> srcLoopIVs;
  getLoopIVs(*srcAccess->opInst, &srcLoopIVs);
  unsigned numSrcLoopIVs = srcLoopIVs.size();
  if (srcLoopDepth > numSrcLoopIVs) {
    srcAccess->opInst->emitError("invalid source loop depth");
    return nullptr;
  }

  // Get loop nest surrounding dst operation.
  SmallVector<ForInst *, 4> dstLoopIVs;
  getLoopIVs(*dstAccess->opInst, &dstLoopIVs);
  unsigned dstLoopIVsSize = dstLoopIVs.size();
  if (dstLoopDepth > dstLoopIVsSize) {
    dstAccess->opInst->emitError("invalid destination loop depth");
    return nullptr;
  }

  // Project out dimensions other than those up to src/dstLoopDepth's.
  dependenceConstraints.projectOut(srcLoopDepth, numSrcLoopIVs - srcLoopDepth);
  dependenceConstraints.projectOut(srcLoopDepth + dstLoopDepth,
                                   dstLoopIVsSize - dstLoopDepth);

  // Set up lower/upper bound affine maps for the slice.
  SmallVector<AffineMap, 4> sliceLbs(srcLoopDepth, AffineMap::Null());
  SmallVector<AffineMap, 4> sliceUbs(srcLoopDepth, AffineMap::Null());

  // Get bounds for src IVs in terms of dst IVs, symbols, and constants.
  dependenceConstraints.getSliceBounds(std::min(srcLoopDepth, numSrcLoopIVs),
                                       srcAccess->opInst->getContext(),
                                       &sliceLbs, &sliceUbs);

  // Set up bound operands for the slice's lower and upper bounds.
  SmallVector<Value *, 4> sliceBoundOperands;
  dependenceConstraints.getIdValues(
      srcLoopDepth, dependenceConstraints.getNumDimAndSymbolIds(),
      &sliceBoundOperands);

  // Find the inst block positions of 'srcAccess->opInst' within 'srcLoopIVs'.
  SmallVector<unsigned, 4> positions;
  // TODO(andydavis): This code is incorrect since srcLoopIVs can be 0-d.
  findInstPosition(srcAccess->opInst, srcLoopIVs[0]->getBlock(), &positions);

  // Clone src loop nest and insert it a the beginning of the instruction block
  // of the loop at 'dstLoopDepth' in 'dstLoopIVs'.
  auto *dstForInst = dstLoopIVs[dstLoopDepth - 1];
  FuncBuilder b(dstForInst->getBody(), dstForInst->getBody()->begin());
  DenseMap<const Value *, Value *> operandMap;
  auto *sliceLoopNest = cast<ForInst>(b.clone(*srcLoopIVs[0], operandMap));

  Instruction *sliceInst =
      getInstAtPosition(positions, /*level=*/0, sliceLoopNest->getBody());
  // Get loop nest surrounding 'sliceInst'.
  SmallVector<ForInst *, 4> sliceSurroundingLoops;
  getLoopIVs(*sliceInst, &sliceSurroundingLoops);

  // Sanity check.
  unsigned sliceSurroundingLoopsSize = sliceSurroundingLoops.size();
  (void)sliceSurroundingLoopsSize;
  assert(dstLoopDepth + numSrcLoopIVs >= sliceSurroundingLoopsSize);
  unsigned sliceLoopLimit = dstLoopDepth + numSrcLoopIVs;
  (void)sliceLoopLimit;
  assert(sliceLoopLimit >= sliceSurroundingLoopsSize);

  // Update loop bounds for loops in 'sliceLoopNest'.
  for (unsigned i = 0; i < srcLoopDepth; ++i) {
    auto *forInst = sliceSurroundingLoops[dstLoopDepth + i];
    if (AffineMap lbMap = sliceLbs[i])
      forInst->setLowerBound(sliceBoundOperands, lbMap);
    if (AffineMap ubMap = sliceUbs[i])
      forInst->setUpperBound(sliceBoundOperands, ubMap);
  }
  return sliceLoopNest;
}

// Constructs  MemRefAccess populating it with the memref, its indices and
// opinst from 'loadOrStoreOpInst'.
MemRefAccess::MemRefAccess(OperationInst *loadOrStoreOpInst) {
  if (auto loadOp = loadOrStoreOpInst->dyn_cast<LoadOp>()) {
    memref = loadOp->getMemRef();
    opInst = loadOrStoreOpInst;
    auto loadMemrefType = loadOp->getMemRefType();
    indices.reserve(loadMemrefType.getRank());
    for (auto *index : loadOp->getIndices()) {
      indices.push_back(index);
    }
  } else {
    assert(loadOrStoreOpInst->isa<StoreOp>() && "load/store op expected");
    auto storeOp = loadOrStoreOpInst->dyn_cast<StoreOp>();
    opInst = loadOrStoreOpInst;
    memref = storeOp->getMemRef();
    auto storeMemrefType = storeOp->getMemRefType();
    indices.reserve(storeMemrefType.getRank());
    for (auto *index : storeOp->getIndices()) {
      indices.push_back(index);
    }
  }
}

/// Returns the nesting depth of this statement, i.e., the number of loops
/// surrounding this statement.
unsigned mlir::getNestingDepth(const Instruction &stmt) {
  const Instruction *currInst = &stmt;
  unsigned depth = 0;
  while ((currInst = currInst->getParentInst())) {
    if (isa<ForInst>(currInst))
      depth++;
  }
  return depth;
}

/// Returns the number of surrounding loops common to 'loopsA' and 'loopsB',
/// where each lists loops from outer-most to inner-most in loop nest.
unsigned mlir::getNumCommonSurroundingLoops(const Instruction &A,
                                            const Instruction &B) {
  SmallVector<ForInst *, 4> loopsA, loopsB;
  getLoopIVs(A, &loopsA);
  getLoopIVs(B, &loopsB);

  unsigned minNumLoops = std::min(loopsA.size(), loopsB.size());
  unsigned numCommonLoops = 0;
  for (unsigned i = 0; i < minNumLoops; ++i) {
    if (loopsA[i] != loopsB[i])
      break;
    ++numCommonLoops;
  }
  return numCommonLoops;
}
