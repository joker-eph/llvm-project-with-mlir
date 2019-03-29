//===- TypeDetail.h - MLIR Type storage details -----------------*- C++ -*-===//
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
// This holds implementation details of Type.
//
//===----------------------------------------------------------------------===//
#ifndef TYPEDETAIL_H_
#define TYPEDETAIL_H_

#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Identifier.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/TypeSupport.h"
#include "mlir/IR/Types.h"

namespace mlir {

class MLIRContext;

namespace detail {

/// Unknown Type Storage and Uniquing.
struct UnknownTypeStorage : public TypeStorage {
  UnknownTypeStorage(Identifier dialectNamespace, StringRef typeData)
      : dialectNamespace(dialectNamespace), typeData(typeData) {}

  /// The hash key used for uniquing.
  using KeyTy = std::pair<Identifier, StringRef>;

  /// Convert to the key type.
  KeyTy getKey() const { return std::make_pair(dialectNamespace, typeData); }

  static UnknownTypeStorage *construct(TypeStorageAllocator &allocator,
                                       const KeyTy &key) {
    StringRef tyData = allocator.copyInto(key.second);
    return new (allocator.allocate<UnknownTypeStorage>())
        UnknownTypeStorage(key.first, tyData);
  }

  // The unknown dialect namespace.
  Identifier dialectNamespace;

  // The parser type data for this unknown type.
  StringRef typeData;
};

/// Integer Type Storage and Uniquing.
struct IntegerTypeStorage : public TypeStorage {
  IntegerTypeStorage(unsigned width) : width(width) {}

  /// The hash key used for uniquing.
  using KeyTy = unsigned;

  /// Convert to the key type.
  KeyTy getKey() const { return width; }

  static IntegerTypeStorage *construct(TypeStorageAllocator &allocator,
                                       KeyTy bitwidth) {
    return new (allocator.allocate<IntegerTypeStorage>())
        IntegerTypeStorage(bitwidth);
  }

  unsigned width;
};

/// Function Type Storage and Uniquing.
struct FunctionTypeStorage : public TypeStorage {
  FunctionTypeStorage(unsigned numInputs, unsigned numResults,
                      Type const *inputsAndResults)
      : TypeStorage(numInputs), numResults(numResults),
        inputsAndResults(inputsAndResults) {}

  /// The hash key used for uniquing.
  using KeyTy = std::pair<ArrayRef<Type>, ArrayRef<Type>>;

  /// Convert to the key type.
  KeyTy getKey() const { return KeyTy(getInputs(), getResults()); }

  /// Construction.
  static FunctionTypeStorage *construct(TypeStorageAllocator &allocator,
                                        const KeyTy &key) {
    ArrayRef<Type> inputs = key.first, results = key.second;

    // Copy the inputs and results into the bump pointer.
    SmallVector<Type, 16> types;
    types.reserve(inputs.size() + results.size());
    types.append(inputs.begin(), inputs.end());
    types.append(results.begin(), results.end());
    auto typesList = allocator.copyInto(ArrayRef<Type>(types));

    // Initialize the memory using placement new.
    return new (allocator.allocate<FunctionTypeStorage>())
        FunctionTypeStorage(inputs.size(), results.size(), typesList.data());
  }

  ArrayRef<Type> getInputs() const {
    return ArrayRef<Type>(inputsAndResults, getSubclassData());
  }
  ArrayRef<Type> getResults() const {
    return ArrayRef<Type>(inputsAndResults + getSubclassData(), numResults);
  }

  unsigned numResults;
  Type const *inputsAndResults;
};

/// VectorOrTensor Type Storage.
struct VectorOrTensorTypeStorage : public TypeStorage {
  VectorOrTensorTypeStorage(Type elementType, unsigned subclassData = 0)
      : TypeStorage(subclassData), elementType(elementType) {}

  /// The hash key used for uniquing.
  using KeyTy = Type;

  /// Convert to the key type.
  KeyTy getKey() const { return elementType; }

  Type elementType;
};

/// Vector Type Storage and Uniquing.
struct VectorTypeStorage : public VectorOrTensorTypeStorage {
  VectorTypeStorage(unsigned shapeSize, Type elementTy,
                    const int *shapeElements)
      : VectorOrTensorTypeStorage(elementTy, shapeSize),
        shapeElements(shapeElements) {}

  /// The hash key used for uniquing.
  using KeyTy = std::pair<ArrayRef<int>, Type>;

  /// Convert to the key type.
  KeyTy getKey() const { return KeyTy(getShape(), elementType); }

  /// Construction.
  static VectorTypeStorage *construct(TypeStorageAllocator &allocator,
                                      const KeyTy &key) {
    // Copy the shape into the bump pointer.
    ArrayRef<int> shape = allocator.copyInto(key.first);

    // Initialize the memory using placement new.
    return new (allocator.allocate<VectorTypeStorage>())
        VectorTypeStorage(shape.size(), key.second, shape.data());
  }

  ArrayRef<int> getShape() const {
    return ArrayRef<int>(shapeElements, getSubclassData());
  }

  const int *shapeElements;
};

struct RankedTensorTypeStorage : public VectorOrTensorTypeStorage {
  RankedTensorTypeStorage(unsigned shapeSize, Type elementTy,
                          const int *shapeElements)
      : VectorOrTensorTypeStorage(elementTy, shapeSize),
        shapeElements(shapeElements) {}

  /// The hash key used for uniquing.
  using KeyTy = std::pair<ArrayRef<int>, Type>;

  /// Convert to the key type.
  KeyTy getKey() const { return KeyTy(getShape(), elementType); }

  /// Construction.
  static RankedTensorTypeStorage *construct(TypeStorageAllocator &allocator,
                                            const KeyTy &key) {
    // Copy the shape into the bump pointer.
    ArrayRef<int> shape = allocator.copyInto(key.first);

    // Initialize the memory using placement new.
    return new (allocator.allocate<RankedTensorTypeStorage>())
        RankedTensorTypeStorage(shape.size(), key.second, shape.data());
  }

  ArrayRef<int> getShape() const {
    return ArrayRef<int>(shapeElements, getSubclassData());
  }

  const int *shapeElements;
};

struct UnrankedTensorTypeStorage : public VectorOrTensorTypeStorage {
  UnrankedTensorTypeStorage(Type elementTy)
      : VectorOrTensorTypeStorage(elementTy) {}

  /// Construction.
  static UnrankedTensorTypeStorage *construct(TypeStorageAllocator &allocator,
                                              Type elementTy) {
    auto *result = allocator.allocate<UnrankedTensorTypeStorage>();
    return new (result) UnrankedTensorTypeStorage(elementTy);
  }
};

struct MemRefTypeStorage : public TypeStorage {
  MemRefTypeStorage(unsigned shapeSize, Type elementType,
                    const int *shapeElements, const unsigned numAffineMaps,
                    AffineMap const *affineMapList, const unsigned memorySpace)
      : TypeStorage(shapeSize), elementType(elementType),
        shapeElements(shapeElements), numAffineMaps(numAffineMaps),
        affineMapList(affineMapList), memorySpace(memorySpace) {}

  /// The hash key used for uniquing.
  // MemRefs are uniqued based on their shape, element type, affine map
  // composition, and memory space.
  using KeyTy = std::tuple<ArrayRef<int>, Type, ArrayRef<AffineMap>, unsigned>;

  /// Convert to the key type.
  KeyTy getKey() const {
    return KeyTy(getShape(), elementType, getAffineMaps(), memorySpace);
  }

  /// Construction.
  static MemRefTypeStorage *construct(TypeStorageAllocator &allocator,
                                      const KeyTy &key) {
    // Copy the shape into the bump pointer.
    ArrayRef<int> shape = allocator.copyInto(std::get<0>(key));

    // Copy the affine map composition into the bump pointer.
    ArrayRef<AffineMap> affineMapComposition =
        allocator.copyInto(std::get<2>(key));

    // Initialize the memory using placement new.
    return new (allocator.allocate<MemRefTypeStorage>())
        MemRefTypeStorage(shape.size(), std::get<1>(key), shape.data(),
                          affineMapComposition.size(),
                          affineMapComposition.data(), std::get<3>(key));
  }

  ArrayRef<int> getShape() const {
    return ArrayRef<int>(shapeElements, getSubclassData());
  }

  ArrayRef<AffineMap> getAffineMaps() const {
    return ArrayRef<AffineMap>(affineMapList, numAffineMaps);
  }

  /// The type of each scalar element of the memref.
  Type elementType;
  /// An array of integers which stores the shape dimension sizes.
  const int *shapeElements;
  /// The number of affine maps in the 'affineMapList' array.
  const unsigned numAffineMaps;
  /// List of affine maps in the memref's layout/index map composition.
  AffineMap const *affineMapList;
  /// Memory space in which data referenced by memref resides.
  const unsigned memorySpace;
};

} // namespace detail
} // namespace mlir
#endif // TYPEDETAIL_H_
