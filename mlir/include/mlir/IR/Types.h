//===- Types.h - MLIR Type Classes ------------------------------*- C++ -*-===//
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

#ifndef MLIR_IR_TYPES_H
#define MLIR_IR_TYPES_H

#include "mlir/IR/TypeSupport.h"
#include "mlir/Support/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMapInfo.h"

namespace llvm {
class fltSemantics;
} // namespace llvm

namespace mlir {
class AffineMap;
class FloatType;
class IndexType;
class IntegerType;
class Location;
class MLIRContext;

namespace detail {
struct TypeStorage;
} // namespace detail

/// Instances of the Type class are immutable and uniqued.  They wrap a pointer
/// to the storage object owned by MLIRContext.  Therefore, instances of Type
/// are passed around by value.
///
/// Some types are "primitives" meaning they do not have any parameters, for
/// example the Index type.  Parametric types have additional information that
/// differentiates the types of the same kind between them, for example the
/// Integer type has bitwidth, making i8 and i16 belong to the same kind by be
/// different instances of the IntegerType.
///
/// Types are constructed and uniqued via the 'detail::TypeUniquer' class.
///
/// Derived type classes are expected to implement several required
/// implementaiton hooks:
///  * Required:
///    - static char typeID;
///      * A unique identifier for this type used during registration.
///
///    - static bool kindof(unsigned kind);
///      * Returns if the provided type kind corresponds to an instance of the
///        current type. Used for isa/dyn_cast casting functionality.
///
///  * Optional:
///    - static using ImplType = ...;
///      * The type alias for the derived storage type. If one is not provided,
///        this defaults to `detail::DefaultTypeStorage’.
///
///
/// Type storage objects inherit from TypeStorage and contain the following:
///    - The type kind (for LLVM-style RTTI);
///    - The abstract descriptor of the type;
///    - Any parameters of the type.
/// For non-parametric types, a convenience DefaultTypeStorage is provided.
/// Parametric storage types must derive TypeStorage and respect the following:
///    - Define a type alias, KeyTy, to a type that uniquely identifies the
///      instance of the type within its kind.
///      * The key type must be constructible from the values passed into the
///        detail::TypeUniquer::get call after the type kind.
///      * The key type must have a llvm::DenseMapInfo specialization for
///        hashing.
///
///    - Provide a method, 'KeyTy getKey() const', to construct the key type
///      from an existing storage instance.
///
///    - Provide a construction method:
///        'DerivedStorage *construct(TypeStorageAllocator &, ...)'
///      that builds a unique instance of the derived storage. The arguments
///      after the TypeStorageAllocator must correspond with the values passed
///      into the detail::TypeUniquer::get call after the type kind.
class Type {
public:
  /// Integer identifier for all the concrete type kinds.
  /// Note: This is not an enum class as each dialect will likely define a
  /// separate enumeration for the specific types that they define. Not being an
  /// enum class also simplifies the handling of type kinds by not requiring
  /// casts for each use.
  enum Kind {
    // Target pointer sized integer, used (e.g.) in affine mappings.
    Index,

    // TensorFlow types.
    TFControl,
    TFResource,
    TFVariant,
    TFComplex64,
    TFComplex128,
    TFF32REF,
    TFString,

    /// These are marker for the first and last 'other' type.
    FIRST_OTHER_TYPE = TFControl,
    LAST_OTHER_TYPE = TFString,

    // Floating point.
    BF16,
    F16,
    F32,
    F64,
    FIRST_FLOATING_POINT_TYPE = BF16,
    LAST_FLOATING_POINT_TYPE = F64,

    // Derived types.
    Integer,
    Function,
    Vector,
    RankedTensor,
    UnrankedTensor,
    MemRef,

    LAST_BUILTIN_TYPE = 0xff,

  // Reserve type kinds for dialect specific type system extensions.
#define DEFINE_TYPE_KIND_RANGE(Dialect)                                        \
  FIRST_##Dialect##_TYPE, LAST_##Dialect##_TYPE = FIRST_##Dialect##_TYPE + 0xff,
#include "DialectTypeRegistry.def"
  };

  using ImplType = detail::TypeStorage;

  Type() : type(nullptr) {}
  /* implicit */ Type(const ImplType *type)
      : type(const_cast<ImplType *>(type)) {}

  Type(const Type &other) : type(other.type) {}
  Type &operator=(Type other) {
    type = other.type;
    return *this;
  }

  bool operator==(Type other) const { return type == other.type; }
  bool operator!=(Type other) const { return !(*this == other); }
  explicit operator bool() const { return type; }

  bool operator!() const { return type == nullptr; }

  template <typename U> bool isa() const;
  template <typename U> U dyn_cast() const;
  template <typename U> U dyn_cast_or_null() const;
  template <typename U> U cast() const;

  /// Return the classification for this type.
  unsigned getKind() const;

  /// Return the LLVMContext in which this type was uniqued.
  MLIRContext *getContext() const;

  /// Get the dialect this type is registered to.
  const Dialect &getDialect() const;

  // Convenience predicates.  This is only for floating point types,
  // derived types should use isa/dyn_cast.
  bool isIndex() const;
  bool isBF16() const;
  bool isF16() const;
  bool isF32() const;
  bool isF64() const;

  /// Return true if this is an integer type with the specified width.
  bool isInteger(unsigned width) const;

  /// Return the bit width of an integer or a float type, assert failure on
  /// other types.
  unsigned getIntOrFloatBitWidth() const;

  /// Return true if this is an integer or index type.
  bool isIntOrIndex() const;
  /// Return true if this is an integer, index, or float type.
  bool isIntOrIndexOrFloat() const;
  /// Return true of this is an integer or a float type.
  bool isIntOrFloat() const;

  // Convenience factories.
  static IndexType getIndex(MLIRContext *ctx);
  static IntegerType getInteger(unsigned width, MLIRContext *ctx);
  static FloatType getBF16(MLIRContext *ctx);
  static FloatType getF16(MLIRContext *ctx);
  static FloatType getF32(MLIRContext *ctx);
  static FloatType getF64(MLIRContext *ctx);

  /// Print the current type.
  void print(raw_ostream &os) const;
  void dump() const;

  friend ::llvm::hash_code hash_value(Type arg);

  unsigned getSubclassData() const;
  void setSubclassData(unsigned val);

  /// Methods for supporting PointerLikeTypeTraits.
  const void *getAsOpaquePointer() const {
    return static_cast<const void *>(type);
  }
  static Type getFromOpaquePointer(const void *pointer) {
    return Type((ImplType *)(pointer));
  }

protected:
  ImplType *type;
};

inline raw_ostream &operator<<(raw_ostream &os, Type type) {
  type.print(os);
  return os;
}

/// Standard Type Utilities.

namespace detail {

struct IntegerTypeStorage;
struct FunctionTypeStorage;
struct VectorOrTensorTypeStorage;
struct VectorTypeStorage;
struct TensorTypeStorage;
struct RankedTensorTypeStorage;
struct UnrankedTensorTypeStorage;
struct MemRefTypeStorage;

} // namespace detail

inline bool Type::isIndex() const { return getKind() == Kind::Index; }
inline bool Type::isBF16() const { return getKind() == Kind::BF16; }
inline bool Type::isF16() const { return getKind() == Kind::F16; }
inline bool Type::isF32() const { return getKind() == Kind::F32; }
inline bool Type::isF64() const { return getKind() == Kind::F64; }

/// Integer types can have arbitrary bitwidth up to a large fixed limit.
class IntegerType : public Type {
public:
  using ImplType = detail::IntegerTypeStorage;
  using Type::Type;

  /// Get or create a new IntegerType of the given width within the context.
  /// Assume the width is within the allowed range and assert on failures.
  /// Use getChecked to handle failures gracefully.
  static IntegerType get(unsigned width, MLIRContext *context);

  /// Get or create a new IntegerType of the given width within the context,
  /// defined at the given, potentially unknown, location.  If the width is
  /// outside the allowed range, emit errors and return a null type.
  static IntegerType getChecked(unsigned width, MLIRContext *context,
                                Location location);

  /// Return the bitwidth of this integer type.
  unsigned getWidth() const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool kindof(unsigned kind) { return kind == Kind::Integer; }

  /// Unique identifier for this type class.
  static char typeID;

  /// Integer representation maximal bitwidth.
  static constexpr unsigned kMaxWidth = 4096;
};

inline IntegerType Type::getInteger(unsigned width, MLIRContext *ctx) {
  return IntegerType::get(width, ctx);
}

/// Return true if this is an integer type with the specified width.
inline bool Type::isInteger(unsigned width) const {
  if (auto intTy = dyn_cast<IntegerType>())
    return intTy.getWidth() == width;
  return false;
}

inline bool Type::isIntOrIndex() const {
  return isa<IndexType>() || isa<IntegerType>();
}

inline bool Type::isIntOrIndexOrFloat() const {
  return isa<IndexType>() || isa<IntegerType>() || isa<FloatType>();
}

inline bool Type::isIntOrFloat() const {
  return isa<IntegerType>() || isa<FloatType>();
}

class FloatType : public Type {
public:
  using Type::Type;

  static FloatType get(Kind kind, MLIRContext *context);

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool kindof(unsigned kind) {
    return kind >= Kind::FIRST_FLOATING_POINT_TYPE &&
           kind <= Kind::LAST_FLOATING_POINT_TYPE;
  }

  /// Return the bitwidth of this float type.
  unsigned getWidth() const;

  /// Return the floating semantics of this float type.
  const llvm::fltSemantics &getFloatSemantics() const;

  /// Unique identifier for this type class.
  static char typeID;
};

inline FloatType Type::getBF16(MLIRContext *ctx) {
  return FloatType::get(Kind::BF16, ctx);
}
inline FloatType Type::getF16(MLIRContext *ctx) {
  return FloatType::get(Kind::F16, ctx);
}
inline FloatType Type::getF32(MLIRContext *ctx) {
  return FloatType::get(Kind::F32, ctx);
}
inline FloatType Type::getF64(MLIRContext *ctx) {
  return FloatType::get(Kind::F64, ctx);
}

/// Index is special integer-like type with unknown platform-dependent bit width
/// used in subscripts and loop induction variables.
class IndexType : public Type {
public:
  using Type::Type;

  /// Crete an IndexType instance, unique in the given context.
  static IndexType get(MLIRContext *context);

  /// Support method to enable LLVM-style type casting.
  static bool kindof(unsigned kind) { return kind == Kind::Index; }

  /// Unique identifier for this type class.
  static char typeID;
};

inline IndexType Type::getIndex(MLIRContext *ctx) {
  return IndexType::get(ctx);
}

/// Function types map from a list of inputs to a list of results.
class FunctionType : public Type {
public:
  using ImplType = detail::FunctionTypeStorage;
  using Type::Type;

  static FunctionType get(ArrayRef<Type> inputs, ArrayRef<Type> results,
                          MLIRContext *context);

  // Input types.
  unsigned getNumInputs() const { return getSubclassData(); }

  Type getInput(unsigned i) const { return getInputs()[i]; }

  ArrayRef<Type> getInputs() const;

  // Result types.
  unsigned getNumResults() const;

  Type getResult(unsigned i) const { return getResults()[i]; }

  ArrayRef<Type> getResults() const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool kindof(unsigned kind) { return kind == Kind::Function; }

  /// Unique identifier for this type class.
  static char typeID;
};

/// This is a common base class between Vector, UnrankedTensor, and RankedTensor
/// types, because many operations work on values of these aggregate types.
class VectorOrTensorType : public Type {
public:
  using ImplType = detail::VectorOrTensorTypeStorage;
  using Type::Type;

  /// Return the element type.
  Type getElementType() const;

  /// If an element type is an integer or a float, return its width.  Abort
  /// otherwise.
  unsigned getElementTypeBitWidth() const;

  /// If this is ranked tensor or vector type, return the number of elements. If
  /// it is an unranked tensor, abort.
  unsigned getNumElements() const;

  /// If this is ranked tensor or vector type, return the rank. If it is an
  /// unranked tensor, return -1.
  int getRank() const;

  /// If this is ranked tensor or vector type, return the shape. If it is an
  /// unranked tensor, abort.
  ArrayRef<int> getShape() const;

  /// If this is unranked tensor or any dimension has unknown size (<0),
  /// it doesn't have static shape. If all dimensions have known size (>= 0),
  /// it has static shape.
  bool hasStaticShape() const;

  /// If this is ranked tensor or vector type, return the size of the specified
  /// dimension. It aborts if the tensor is unranked (this can be checked by
  /// the getRank call method).
  int getDimSize(unsigned i) const;

  /// Get the total amount of bits occupied by a value of this type.  This does
  /// not take into account any memory layout or widening constraints, e.g. a
  /// vector<3xi57> is reported to occupy 3x57=171 bit, even though in practice
  /// it will likely be stored as in a 4xi64 vector register.  Fail an assertion
  /// if the size cannot be computed statically, i.e. if the tensor has a
  /// dynamic shape or if its elemental type does not have a known bit width.
  long getSizeInBits() const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool kindof(unsigned kind) {
    return kind == Kind::Vector || kind == Kind::RankedTensor ||
           kind == Kind::UnrankedTensor;
  }
};

/// Vector types represent multi-dimensional SIMD vectors, and have a fixed
/// known constant shape with one or more dimension.
class VectorType : public VectorOrTensorType {
public:
  using ImplType = detail::VectorTypeStorage;
  using VectorOrTensorType::VectorOrTensorType;

  /// Get or create a new VectorType of the provided shape and element type.
  /// Assumes the arguments define a well-formed VectorType.
  static VectorType get(ArrayRef<int> shape, Type elementType);

  /// Get or create a new VectorType of the provided shape and element type
  /// declared at the given, potentially unknown, location.  If the VectorType
  /// defined by the arguments would be ill-formed, emit errors and return
  /// nullptr-wrapping type.
  static VectorType getChecked(ArrayRef<int> shape, Type elementType,
                               Location location);

  /// Returns true of the given type can be used as an element of a vector type.
  /// In particular, vectors can consist of integer or float primitives.
  static bool isValidElementType(Type t) { return t.isIntOrFloat(); }

  ArrayRef<int> getShape() const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool kindof(unsigned kind) { return kind == Kind::Vector; }

  /// Unique identifier for this type class.
  static char typeID;
};

/// Tensor types represent multi-dimensional arrays, and have two variants:
/// RankedTensorType and UnrankedTensorType.
class TensorType : public VectorOrTensorType {
public:
  using ImplType = detail::TensorTypeStorage;
  using VectorOrTensorType::VectorOrTensorType;

  /// Return true if the specified element type is ok in a tensor.
  static bool isValidElementType(Type type) {
    // TODO(riverriddle): TensorFlow types are currently considered valid for
    // legacy reasons.
    return type.isIntOrFloat() || type.isa<VectorType>() ||
           (type.getKind() >=
                static_cast<unsigned>(Kind::FIRST_TENSORFLOW_TYPE) &&
            type.getKind() <=
                static_cast<unsigned>(Kind::LAST_TENSORFLOW_TYPE));
  }

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool kindof(unsigned kind) {
    return kind == Kind::RankedTensor || kind == Kind::UnrankedTensor;
  }
};

/// Ranked tensor types represent multi-dimensional arrays that have a shape
/// with a fixed number of dimensions. Each shape element can be a positive
/// integer or unknown (represented -1).
class RankedTensorType : public TensorType {
public:
  using ImplType = detail::RankedTensorTypeStorage;
  using TensorType::TensorType;

  /// Get or create a new RankedTensorType of the provided shape and element
  /// type. Assumes the arguments define a well-formed type.
  static RankedTensorType get(ArrayRef<int> shape, Type elementType);

  /// Get or create a new RankedTensorType of the provided shape and element
  /// type declared at the given, potentially unknown, location.  If the
  /// RankedTensorType defined by the arguments would be ill-formed, emit errors
  /// and return a nullptr-wrapping type.
  static RankedTensorType getChecked(ArrayRef<int> shape, Type elementType,
                                     Location location);

  ArrayRef<int> getShape() const;

  static bool kindof(unsigned kind) { return kind == Kind::RankedTensor; }

  /// Unique identifier for this type class.
  static char typeID;
};

/// Unranked tensor types represent multi-dimensional arrays that have an
/// unknown shape.
class UnrankedTensorType : public TensorType {
public:
  using ImplType = detail::UnrankedTensorTypeStorage;
  using TensorType::TensorType;

  /// Get or create a new UnrankedTensorType of the provided shape and element
  /// type. Assumes the arguments define a well-formed type.
  static UnrankedTensorType get(Type elementType);

  /// Get or create a new UnrankedTensorType of the provided shape and element
  /// type declared at the given, potentially unknown, location.  If the
  /// UnrankedTensorType defined by the arguments would be ill-formed, emit
  /// errors and return a nullptr-wrapping type.
  static UnrankedTensorType getChecked(Type elementType, Location location);

  ArrayRef<int> getShape() const { return ArrayRef<int>(); }

  static bool kindof(unsigned kind) { return kind == Kind::UnrankedTensor; }

  /// Unique identifier for this type class.
  static char typeID;
};

/// MemRef types represent a region of memory that have a shape with a fixed
/// number of dimensions. Each shape element can be a positive integer or
/// unknown (represented by any negative integer). MemRef types also have an
/// affine map composition, represented as an array AffineMap pointers.
class MemRefType : public Type {
public:
  using ImplType = detail::MemRefTypeStorage;
  using Type::Type;

  /// Get or create a new MemRefType based on shape, element type, affine
  /// map composition, and memory space.  Assumes the arguments define a
  /// well-formed MemRef type.  Use getChecked to gracefully handle MemRefType
  /// construction failures.
  static MemRefType get(ArrayRef<int> shape, Type elementType,
                        ArrayRef<AffineMap> affineMapComposition,
                        unsigned memorySpace);

  /// Get or create a new MemRefType based on shape, element type, affine
  /// map composition, and memory space declared at the given location.
  /// If the location is unknown, the last argument should be an instance of
  /// UnknownLoc.  If the MemRefType defined by the arguments would be
  /// ill-formed, emits errors (to the handler registered with the context or to
  /// the error stream) and returns nullptr.
  static MemRefType getChecked(ArrayRef<int> shape, Type elementType,
                               ArrayRef<AffineMap> affineMapComposition,
                               unsigned memorySpace, Location location);

  unsigned getRank() const { return getShape().size(); }

  /// Returns an array of memref shape dimension sizes.
  ArrayRef<int> getShape() const;

  /// Return the size of the specified dimension, or -1 if unspecified.
  int getDimSize(unsigned i) const { return getShape()[i]; }

  /// Returns the elemental type for this memref shape.
  Type getElementType() const;

  /// Returns an array of affine map pointers representing the memref affine
  /// map composition.
  ArrayRef<AffineMap> getAffineMaps() const;

  /// Returns the memory space in which data referred to by this memref resides.
  unsigned getMemorySpace() const;

  /// Returns the number of dimensions with dynamic size.
  unsigned getNumDynamicDims() const;

  static bool kindof(unsigned kind) { return kind == Kind::MemRef; }

  /// Unique identifier for this type class.
  static char typeID;

private:
  static MemRefType getSafe(ArrayRef<int> shape, Type elementType,
                            ArrayRef<AffineMap> affineMapComposition,
                            unsigned memorySpace, Optional<Location> location);
};

// Make Type hashable.
inline ::llvm::hash_code hash_value(Type arg) {
  return ::llvm::hash_value(arg.type);
}

template <typename U> bool Type::isa() const {
  assert(type && "isa<> used on a null type.");
  return U::kindof(getKind());
}
template <typename U> U Type::dyn_cast() const {
  return isa<U>() ? U(type) : U(nullptr);
}
template <typename U> U Type::dyn_cast_or_null() const {
  return (type && isa<U>()) ? U(type) : U(nullptr);
}
template <typename U> U Type::cast() const {
  assert(isa<U>());
  return U(type);
}

} // end namespace mlir

namespace llvm {

// Type hash just like pointers.
template <> struct DenseMapInfo<mlir::Type> {
  static mlir::Type getEmptyKey() {
    auto pointer = llvm::DenseMapInfo<void *>::getEmptyKey();
    return mlir::Type(static_cast<mlir::Type::ImplType *>(pointer));
  }
  static mlir::Type getTombstoneKey() {
    auto pointer = llvm::DenseMapInfo<void *>::getTombstoneKey();
    return mlir::Type(static_cast<mlir::Type::ImplType *>(pointer));
  }
  static unsigned getHashValue(mlir::Type val) { return mlir::hash_value(val); }
  static bool isEqual(mlir::Type LHS, mlir::Type RHS) { return LHS == RHS; }
};

/// We align TypeStorage by 8, so allow LLVM to steal the low bits.
template <> struct PointerLikeTypeTraits<mlir::Type> {
public:
  static inline void *getAsVoidPointer(mlir::Type I) {
    return const_cast<void *>(I.getAsOpaquePointer());
  }
  static inline mlir::Type getFromVoidPointer(void *P) {
    return mlir::Type::getFromOpaquePointer(P);
  }
  enum { NumLowBitsAvailable = 3 };
};

} // namespace llvm

#endif // MLIR_IR_TYPES_H
