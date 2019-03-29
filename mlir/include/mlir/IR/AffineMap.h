//===- AffineMap.h - MLIR Affine Map Class ----------------------*- C++ -*-===//
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
// Affine maps are mathematical functions which map a list of dimension
// identifiers and symbols, to multidimensional affine expressions.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_IR_AFFINE_MAP_H
#define MLIR_IR_AFFINE_MAP_H

#include "mlir/Support/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMapInfo.h"

namespace mlir {

namespace detail {
class AffineMapStorage;
} // end namespace detail

class AffineExpr;
class Attribute;
class MLIRContext;

/// A multi-dimensional affine map
/// Affine map's are immutable like Type's, and they are uniqued.
/// Eg: (d0, d1) -> (d0/128, d0 mod 128, d1)
/// The names used (d0, d1) don't matter - it's the mathematical function that
/// is unique to this affine map.
class AffineMap {
public:
  using ImplType = detail::AffineMapStorage;

  explicit AffineMap(ImplType *map = nullptr) : map(map) {}
  static AffineMap Null() { return AffineMap(nullptr); }

  static AffineMap get(unsigned dimCount, unsigned symbolCount,
                       ArrayRef<AffineExpr> results,
                       ArrayRef<AffineExpr> rangeSizes);

  /// Returns a single constant result affine map.
  static AffineMap getConstantMap(int64_t val, MLIRContext *context);

  MLIRContext *getContext() const;

  explicit operator bool() { return map; }
  bool operator==(const AffineMap &other) const { return other.map == map; }

  /// Returns true if the co-domain (or more loosely speaking, range) of this
  /// map is bounded. Bounded affine maps have a size (extent) for each of
  /// their range dimensions (more accurately co-domain dimensions).
  bool isBounded() const;

  /// Returns true if this affine map is an identity affine map.
  /// An identity affine map corresponds to an identity affine function on the
  /// dimensional identifiers.
  bool isIdentity() const;

  /// Returns true if this affine map is a single result constant function.
  bool isSingleConstant() const;

  /// Returns the constant result of this map. This methods asserts that the map
  /// has a single constant result.
  int64_t getSingleConstantResult() const;

  // Prints affine map to 'os'.
  void print(raw_ostream &os) const;
  void dump() const;

  unsigned getNumDims() const;
  unsigned getNumSymbols() const;
  unsigned getNumResults() const;
  unsigned getNumInputs() const;

  ArrayRef<AffineExpr> getResults() const;
  AffineExpr getResult(unsigned idx) const;

  ArrayRef<AffineExpr> getRangeSizes() const;

  /// Folds the results of the application of an affine map on the provided
  /// operands to a constant if possible. Returns false if the folding happens,
  /// true otherwise.
  bool constantFold(ArrayRef<Attribute *> operandConstants,
                    SmallVectorImpl<Attribute *> &results) const;

  friend ::llvm::hash_code hash_value(AffineMap arg);

private:
  ImplType *map;
};

// Make AffineExpr hashable.
inline ::llvm::hash_code hash_value(AffineMap arg) {
  return ::llvm::hash_value(arg.map);
}

} // end namespace mlir

namespace llvm {

// AffineExpr hash just like pointers
template <> struct DenseMapInfo<mlir::AffineMap> {
  static mlir::AffineMap getEmptyKey() {
    auto pointer = llvm::DenseMapInfo<void *>::getEmptyKey();
    return mlir::AffineMap(static_cast<mlir::AffineMap::ImplType *>(pointer));
  }
  static mlir::AffineMap getTombstoneKey() {
    auto pointer = llvm::DenseMapInfo<void *>::getTombstoneKey();
    return mlir::AffineMap(static_cast<mlir::AffineMap::ImplType *>(pointer));
  }
  static unsigned getHashValue(mlir::AffineMap val) {
    return mlir::hash_value(val);
  }
  static bool isEqual(mlir::AffineMap LHS, mlir::AffineMap RHS) {
    return LHS == RHS;
  }
};

} // namespace llvm

#endif // MLIR_IR_AFFINE_MAP_H
