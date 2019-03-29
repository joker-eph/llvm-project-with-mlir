//===- Utils.h - General analysis utilities ---------------------*- C++ -*-===//
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
// This header file defines prototypes for various transformation utilities for
// memref's and non-loop IR structures. These are not passes by themselves but
// are used either by passes, optimization sequences, or in turn by other
// transformation utilities.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_ANALYSIS_UTILS_H
#define MLIR_ANALYSIS_UTILS_H

#include "mlir/IR/AffineMap.h"
#include "mlir/IR/AffineStructures.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/Location.h"
#include "mlir/Support/LLVM.h"
#include "llvm/ADT/SmallVector.h"
#include <memory>

namespace mlir {

class AffineForOp;
template <typename T> class ConstOpPointer;
class Block;
class FlatAffineConstraints;
class Instruction;
class Location;
class MemRefAccess;
template <typename T> class OpPointer;
class Instruction;
class Value;

/// Returns true if instruction 'a' dominates instruction b.
bool dominates(const Instruction &a, const Instruction &b);

/// Returns true if instruction 'a' properly dominates instruction b.
bool properlyDominates(const Instruction &a, const Instruction &b);

/// Populates 'loops' with IVs of the loops surrounding 'inst' ordered from
/// the outermost 'for' instruction to the innermost one.
//  TODO(bondhugula): handle 'if' inst's.
void getLoopIVs(const Instruction &inst,
                SmallVectorImpl<OpPointer<AffineForOp>> *loops);

/// Returns the nesting depth of this instruction, i.e., the number of loops
/// surrounding this instruction.
unsigned getNestingDepth(const Instruction &stmt);

/// ComputationSliceState aggregates loop bound AffineMaps and their associated
/// operands for a set of loops within a loop nest (typically the set of loops
/// surrounding a store operation). Loop bound AffineMaps which are non-null
/// represent slices of that loop's iteration space.
struct ComputationSliceState {
  // List of lower bound AffineMaps.
  SmallVector<AffineMap, 4> lbs;
  // List of upper bound AffineMaps.
  SmallVector<AffineMap, 4> ubs;
  // List of lower bound operands (lbOperands[i] are used by 'lbs[i]').
  std::vector<SmallVector<Value *, 4>> lbOperands;
  // List of upper bound operands (ubOperands[i] are used by 'ubs[i]').
  std::vector<SmallVector<Value *, 4>> ubOperands;
};

/// Computes computation slice loop bounds for the loop nest surrounding
/// 'srcAccess', where the returned loop bound AffineMaps are functions of
/// loop IVs from the loop nest surrounding 'dstAccess'.
/// Returns true on success, false otherwise.
bool getBackwardComputationSliceState(const MemRefAccess &srcAccess,
                                      const MemRefAccess &dstAccess,
                                      unsigned dstLoopDepth,
                                      ComputationSliceState *sliceState);

/// Creates a clone of the computation contained in the loop nest surrounding
/// 'srcOpInst', slices the iteration space of src loop based on slice bounds
/// in 'sliceState', and inserts the computation slice at the beginning of the
/// instruction block of the loop at 'dstLoopDepth' in the loop nest surrounding
/// 'dstOpInst'. Returns the top-level loop of the computation slice on
/// success, returns nullptr otherwise.
// Loop depth is a crucial optimization choice that determines where to
// materialize the results of the backward slice - presenting a trade-off b/w
// storage and redundant computation in several cases.
// TODO(andydavis) Support computation slices with common surrounding loops.
OpPointer<AffineForOp>
insertBackwardComputationSlice(Instruction *srcOpInst, Instruction *dstOpInst,
                               unsigned dstLoopDepth,
                               ComputationSliceState *sliceState);

/// A region of a memref's data space; this is typically constructed by
/// analyzing load/store op's on this memref and the index space of loops
/// surrounding such op's.
// For example, the memref region for a load operation at loop depth = 1:
//
//    for %i = 0 to 32 {
//      for %ii = %i to (d0) -> (d0 + 8) (%i) {
//        load %A[%ii]
//      }
//    }
//
// Region:  {memref = %A, write = false, {%i <= m0 <= %i + 7} }
// The last field is a 2-d FlatAffineConstraints symbolic in %i.
//
struct MemRefRegion {
  explicit MemRefRegion(Location loc) : loc(loc) {}

  /// Computes the memory region accessed by this memref with the region
  /// represented as constraints symbolic/parameteric in 'loopDepth' loops
  /// surrounding opInst. Returns false if this fails due to yet unimplemented
  /// cases. The computed region's 'cst' field has exactly as many dimensional
  /// identifiers as the rank of the memref, and *potentially* additional
  /// symbolic identifiers which could include any of the loop IVs surrounding
  /// opInst up until 'loopDepth' and another additional Function symbols
  /// involved with the access (for eg., those appear in affine.apply's, loop
  /// bounds, etc.). If 'sliceState' is non-null, operands from 'sliceState'
  /// are added as symbols, and the following constraints are added to the
  /// system:
  /// *) Inequality constraints which represent loop bounds for 'sliceState'
  ///    operands which are loop IVS (these represent the destination loop IVs
  ///    of the slice, and are added as symbols to MemRefRegion's constraint
  ///    system).
  /// *) Inequality constraints for the slice bounds in 'sliceState', which
  ///    represent the bounds on the loop IVs in this constraint system w.r.t
  ///    to slice operands (which correspond to symbols).
  ///
  ///  For example, the memref region for this operation at loopDepth = 1 will
  ///  be:
  ///
  ///    for %i = 0 to 32 {
  ///      for %ii = %i to (d0) -> (d0 + 8) (%i) {
  ///        load %A[%ii]
  ///      }
  ///    }
  ///
  ///   {memref = %A, write = false, {%i <= m0 <= %i + 7} }
  /// The last field is a 2-d FlatAffineConstraints symbolic in %i.
  ///
  bool compute(Instruction *inst, unsigned loopDepth,
               ComputationSliceState *sliceState = nullptr);

  FlatAffineConstraints *getConstraints() { return &cst; }
  const FlatAffineConstraints *getConstraints() const { return &cst; }
  bool isWrite() const { return write; }
  void setWrite(bool flag) { write = flag; }

  /// Returns a constant upper bound on the number of elements in this region if
  /// bounded by a known constant (always possible for static shapes), None
  /// otherwise. Note that the symbols of the region are treated specially,
  /// i.e., the returned bounding constant holds for *any given* value of the
  /// symbol identifiers. The 'shape' vector is set to the corresponding
  /// dimension-wise bounds major to minor. We use int64_t instead of uint64_t
  /// since index types can be at most int64_t.
  Optional<int64_t> getConstantBoundingSizeAndShape(
      SmallVectorImpl<int64_t> *shape = nullptr,
      std::vector<SmallVector<int64_t, 4>> *lbs = nullptr,
      SmallVectorImpl<int64_t> *lbDivisors = nullptr) const;

  /// A wrapper around FlatAffineConstraints::getConstantBoundOnDimSize(). 'pos'
  /// corresponds to the position of the memref shape's dimension (major to
  /// minor) which matches 1:1 with the dimensional identifier positions in
  //'cst'.
  Optional<int64_t>
  getConstantBoundOnDimSize(unsigned pos,
                            SmallVectorImpl<int64_t> *lb = nullptr,
                            int64_t *lbDivisor = nullptr) const {
    assert(pos < getRank() && "invalid position");
    return cst.getConstantBoundOnDimSize(pos, lb);
  }

  /// Returns the size of this MemRefRegion in bytes.
  Optional<int64_t> getRegionSize();

  bool unionBoundingBox(const MemRefRegion &other);

  /// Returns the rank of the memref that this region corresponds to.
  unsigned getRank() const;

  /// Memref that this region corresponds to.
  Value *memref;

  /// Read or write.
  bool write;

  /// If there is more than one load/store op associated with the region, the
  /// location information would correspond to one of those op's.
  Location loc;

  /// Region (data space) of the memref accessed. This set will thus have at
  /// least as many dimensional identifiers as the shape dimensionality of the
  /// memref, and these are the leading dimensions of the set appearing in that
  /// order (major to minor / outermost to innermost). There may be additional
  /// identifiers since getMemRefRegion() is called with a specific loop depth,
  /// and thus the region is symbolic in the outer surrounding loops at that
  /// depth.
  // TODO(bondhugula): Replace this to exploit HyperRectangularSet.
  FlatAffineConstraints cst;
};

/// Returns the size of memref data in bytes if it's statically shaped, None
/// otherwise.
Optional<uint64_t> getMemRefSizeInBytes(MemRefType memRefType);

/// Checks a load or store op for an out of bound access; returns true if the
/// access is out of bounds along any of the dimensions, false otherwise. Emits
/// a diagnostic error (with location information) if emitError is true.
template <typename LoadOrStoreOpPointer>
bool boundCheckLoadOrStoreOp(LoadOrStoreOpPointer loadOrStoreOp,
                             bool emitError = true);

/// Returns the number of surrounding loops common to both A and B.
unsigned getNumCommonSurroundingLoops(const Instruction &A,
                                      const Instruction &B);

/// Gets the memory footprint of all data touched in the specified memory space
/// in bytes; if the memory space is unspecified, considers all memory spaces.
Optional<int64_t> getMemoryFootprintBytes(ConstOpPointer<AffineForOp> forOp,
                                          int memorySpace = -1);
} // end namespace mlir

#endif // MLIR_ANALYSIS_UTILS_H
