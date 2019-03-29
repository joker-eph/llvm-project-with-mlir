//===- MLIRContext.cpp - MLIR Type Classes --------------------------------===//
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

#include "mlir/IR/MLIRContext.h"
#include "AffineExprDetail.h"
#include "AffineMapDetail.h"
#include "AttributeDetail.h"
#include "AttributeListStorage.h"
#include "IntegerSetDetail.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/Identifier.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/Types.h"
#include "mlir/Support/MathExtras.h"
#include "mlir/Support/STLExtras.h"
#include "third_party/llvm/llvm/include/llvm/ADT/STLExtras.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>

using namespace mlir;
using namespace mlir::detail;
using namespace llvm;

namespace {
struct FunctionTypeKeyInfo : DenseMapInfo<FunctionType *> {
  // Functions are uniqued based on their inputs and results.
  using KeyTy = std::pair<ArrayRef<Type *>, ArrayRef<Type *>>;
  using DenseMapInfo<FunctionType *>::getHashValue;
  using DenseMapInfo<FunctionType *>::isEqual;

  static unsigned getHashValue(KeyTy key) {
    return hash_combine(
        hash_combine_range(key.first.begin(), key.first.end()),
        hash_combine_range(key.second.begin(), key.second.end()));
  }

  static bool isEqual(const KeyTy &lhs, const FunctionType *rhs) {
    if (rhs == getEmptyKey() || rhs == getTombstoneKey())
      return false;
    return lhs == KeyTy(rhs->getInputs(), rhs->getResults());
  }
};

struct AffineMapKeyInfo : DenseMapInfo<AffineMap> {
  // Affine maps are uniqued based on their dim/symbol counts and affine
  // expressions.
  using KeyTy = std::tuple<unsigned, unsigned, ArrayRef<AffineExpr>,
                           ArrayRef<AffineExpr>>;
  using DenseMapInfo<AffineMap>::getHashValue;
  using DenseMapInfo<AffineMap>::isEqual;

  static unsigned getHashValue(KeyTy key) {
    return hash_combine(
        std::get<0>(key), std::get<1>(key),
        hash_combine_range(std::get<2>(key).begin(), std::get<2>(key).end()),
        hash_combine_range(std::get<3>(key).begin(), std::get<3>(key).end()));
  }

  static bool isEqual(const KeyTy &lhs, AffineMap rhs) {
    if (rhs == getEmptyKey() || rhs == getTombstoneKey())
      return false;
    return lhs == std::make_tuple(rhs.getNumDims(), rhs.getNumSymbols(),
                                  rhs.getResults(), rhs.getRangeSizes());
  }
};

struct IntegerSetKeyInfo : DenseMapInfo<IntegerSet> {
  // Integer sets are uniqued based on their dim/symbol counts, affine
  // expressions appearing in the LHS of constraints, and eqFlags.
  using KeyTy =
      std::tuple<unsigned, unsigned, ArrayRef<AffineExpr>, ArrayRef<bool>>;
  using DenseMapInfo<IntegerSet>::getHashValue;
  using DenseMapInfo<IntegerSet>::isEqual;

  static unsigned getHashValue(KeyTy key) {
    return hash_combine(
        std::get<0>(key), std::get<1>(key),
        hash_combine_range(std::get<2>(key).begin(), std::get<2>(key).end()),
        hash_combine_range(std::get<3>(key).begin(), std::get<3>(key).end()));
  }

  static bool isEqual(const KeyTy &lhs, IntegerSet rhs) {
    if (rhs == getEmptyKey() || rhs == getTombstoneKey())
      return false;
    return lhs == std::make_tuple(rhs.getNumDims(), rhs.getNumSymbols(),
                                  rhs.getConstraints(), rhs.getEqFlags());
  }
};

struct VectorTypeKeyInfo : DenseMapInfo<VectorType *> {
  // Vectors are uniqued based on their element type and shape.
  using KeyTy = std::pair<Type *, ArrayRef<int>>;
  using DenseMapInfo<VectorType *>::getHashValue;
  using DenseMapInfo<VectorType *>::isEqual;

  static unsigned getHashValue(KeyTy key) {
    return hash_combine(
        DenseMapInfo<Type *>::getHashValue(key.first),
        hash_combine_range(key.second.begin(), key.second.end()));
  }

  static bool isEqual(const KeyTy &lhs, const VectorType *rhs) {
    if (rhs == getEmptyKey() || rhs == getTombstoneKey())
      return false;
    return lhs == KeyTy(rhs->getElementType(), rhs->getShape());
  }
};

struct RankedTensorTypeKeyInfo : DenseMapInfo<RankedTensorType *> {
  // Ranked tensors are uniqued based on their element type and shape.
  using KeyTy = std::pair<Type *, ArrayRef<int>>;
  using DenseMapInfo<RankedTensorType *>::getHashValue;
  using DenseMapInfo<RankedTensorType *>::isEqual;

  static unsigned getHashValue(KeyTy key) {
    return hash_combine(
        DenseMapInfo<Type *>::getHashValue(key.first),
        hash_combine_range(key.second.begin(), key.second.end()));
  }

  static bool isEqual(const KeyTy &lhs, const RankedTensorType *rhs) {
    if (rhs == getEmptyKey() || rhs == getTombstoneKey())
      return false;
    return lhs == KeyTy(rhs->getElementType(), rhs->getShape());
  }
};

struct MemRefTypeKeyInfo : DenseMapInfo<MemRefType *> {
  // MemRefs are uniqued based on their element type, shape, affine map
  // composition, and memory space.
  using KeyTy =
      std::tuple<Type *, ArrayRef<int>, ArrayRef<AffineMap>, unsigned>;
  using DenseMapInfo<MemRefType *>::getHashValue;
  using DenseMapInfo<MemRefType *>::isEqual;

  static unsigned getHashValue(KeyTy key) {
    return hash_combine(
        DenseMapInfo<Type *>::getHashValue(std::get<0>(key)),
        hash_combine_range(std::get<1>(key).begin(), std::get<1>(key).end()),
        hash_combine_range(std::get<2>(key).begin(), std::get<2>(key).end()),
        std::get<3>(key));
  }

  static bool isEqual(const KeyTy &lhs, const MemRefType *rhs) {
    if (rhs == getEmptyKey() || rhs == getTombstoneKey())
      return false;
    return lhs == std::make_tuple(rhs->getElementType(), rhs->getShape(),
                                  rhs->getAffineMaps(), rhs->getMemorySpace());
  }
};

struct FloatAttrKeyInfo : DenseMapInfo<FloatAttributeStorage *> {
  // Float attributes are uniqued based on wrapped APFloat.
  using KeyTy = APFloat;
  using DenseMapInfo<FloatAttributeStorage *>::getHashValue;
  using DenseMapInfo<FloatAttributeStorage *>::isEqual;

  static unsigned getHashValue(KeyTy key) { return llvm::hash_value(key); }

  static bool isEqual(const KeyTy &lhs, const FloatAttributeStorage *rhs) {
    if (rhs == getEmptyKey() || rhs == getTombstoneKey())
      return false;
    return lhs.bitwiseIsEqual(rhs->getValue());
  }
};

struct ArrayAttrKeyInfo : DenseMapInfo<ArrayAttributeStorage *> {
  // Array attributes are uniqued based on their elements.
  using KeyTy = ArrayRef<Attribute>;
  using DenseMapInfo<ArrayAttributeStorage *>::getHashValue;
  using DenseMapInfo<ArrayAttributeStorage *>::isEqual;

  static unsigned getHashValue(KeyTy key) {
    return hash_combine_range(key.begin(), key.end());
  }

  static bool isEqual(const KeyTy &lhs, const ArrayAttributeStorage *rhs) {
    if (rhs == getEmptyKey() || rhs == getTombstoneKey())
      return false;
    return lhs == rhs->value;
  }
};

struct AttributeListKeyInfo : DenseMapInfo<AttributeListStorage *> {
  // Array attributes are uniqued based on their elements.
  using KeyTy = ArrayRef<NamedAttribute>;
  using DenseMapInfo<AttributeListStorage *>::getHashValue;
  using DenseMapInfo<AttributeListStorage *>::isEqual;

  static unsigned getHashValue(KeyTy key) {
    return hash_combine_range(key.begin(), key.end());
  }

  static bool isEqual(const KeyTy &lhs, const AttributeListStorage *rhs) {
    if (rhs == getEmptyKey() || rhs == getTombstoneKey())
      return false;
    return lhs == rhs->getElements();
  }
};

struct DenseElementsAttrInfo : DenseMapInfo<DenseElementsAttributeStorage *> {
  using KeyTy = std::pair<VectorOrTensorType *, ArrayRef<char>>;
  using DenseMapInfo<DenseElementsAttributeStorage *>::getHashValue;
  using DenseMapInfo<DenseElementsAttributeStorage *>::isEqual;

  static unsigned getHashValue(KeyTy key) {
    return hash_combine(
        key.first, hash_combine_range(key.second.begin(), key.second.end()));
  }

  static bool isEqual(const KeyTy &lhs,
                      const DenseElementsAttributeStorage *rhs) {
    if (rhs == getEmptyKey() || rhs == getTombstoneKey())
      return false;
    return lhs == std::make_pair(rhs->type, rhs->data);
  }
};

struct OpaqueElementsAttrInfo : DenseMapInfo<OpaqueElementsAttributeStorage *> {
  using KeyTy = std::pair<VectorOrTensorType *, StringRef>;
  using DenseMapInfo<OpaqueElementsAttributeStorage *>::getHashValue;
  using DenseMapInfo<OpaqueElementsAttributeStorage *>::isEqual;

  static unsigned getHashValue(KeyTy key) {
    return hash_combine(
        key.first, hash_combine_range(key.second.begin(), key.second.end()));
  }

  static bool isEqual(const KeyTy &lhs,
                      const OpaqueElementsAttributeStorage *rhs) {
    if (rhs == getEmptyKey() || rhs == getTombstoneKey())
      return false;
    return lhs == std::make_pair(rhs->type, rhs->bytes);
  }
};
} // end anonymous namespace.

namespace mlir {
/// This is the implementation of the MLIRContext class, using the pImpl idiom.
/// This class is completely private to this file, so everything is public.
class MLIRContextImpl {
public:
  /// We put location info into this allocator, since it is generally not
  /// touched by compiler passes.
  llvm::BumpPtrAllocator locationAllocator;

  /// The singleton for UnknownLoc.
  UnknownLoc *theUnknownLoc = nullptr;

  /// These are filename locations uniqued into this MLIRContext.
  llvm::StringMap<char, llvm::BumpPtrAllocator &> filenames;

  /// FileLineColLoc uniquing.
  DenseMap<std::tuple<const char *, unsigned, unsigned>, FileLineColLoc *>
      fileLineColLocs;

  /// We put immortal objects into this allocator.
  llvm::BumpPtrAllocator allocator;

  /// This is the handler to use to report diagnostics, or null if not
  /// registered.
  MLIRContext::DiagnosticHandlerTy diagnosticHandler;

  /// This is a list of dialects that are created referring to this context.
  /// The MLIRContext owns the objects.
  std::vector<std::unique_ptr<Dialect>> dialects;

  /// This is a mapping from operation name to AbstractOperation for registered
  /// operations.
  StringMap<AbstractOperation> registeredOperations;

  /// These are identifiers uniqued into this MLIRContext.
  llvm::StringMap<char, llvm::BumpPtrAllocator &> identifiers;

  // Uniquing table for 'other' types.
  OtherType *otherTypes[int(Type::Kind::LAST_OTHER_TYPE) -
                        int(Type::Kind::FIRST_OTHER_TYPE) + 1] = {nullptr};

  // Uniquing table for 'float' types.
  FloatType *floatTypes[int(Type::Kind::LAST_FLOATING_POINT_TYPE) -
                        int(Type::Kind::FIRST_FLOATING_POINT_TYPE) + 1] = {
      nullptr};

  // Affine map uniquing.
  using AffineMapSet = DenseSet<AffineMap, AffineMapKeyInfo>;
  AffineMapSet affineMaps;

  // Integer set uniquing.
  using IntegerSets = DenseSet<IntegerSet, IntegerSetKeyInfo>;
  IntegerSets integerSets;

  // Affine binary op expression uniquing. Figure out uniquing of dimensional
  // or symbolic identifiers.
  DenseMap<std::tuple<unsigned, AffineExpr, AffineExpr>, AffineExpr>
      affineExprs;

  // Uniqui'ing of AffineDimExpr, AffineSymbolExpr's by their position.
  std::vector<AffineDimExprStorage *> dimExprs;
  std::vector<AffineSymbolExprStorage *> symbolExprs;

  // Uniqui'ing of AffineConstantExprStorage using constant value as key.
  DenseMap<int64_t, AffineConstantExprStorage *> constExprs;

  /// Integer type uniquing.
  DenseMap<unsigned, IntegerType *> integers;

  /// Function type uniquing.
  using FunctionTypeSet = DenseSet<FunctionType *, FunctionTypeKeyInfo>;
  FunctionTypeSet functions;

  /// Vector type uniquing.
  using VectorTypeSet = DenseSet<VectorType *, VectorTypeKeyInfo>;
  VectorTypeSet vectors;

  /// Ranked tensor type uniquing.
  using RankedTensorTypeSet =
      DenseSet<RankedTensorType *, RankedTensorTypeKeyInfo>;
  RankedTensorTypeSet rankedTensors;

  /// Unranked tensor type uniquing.
  DenseMap<Type *, UnrankedTensorType *> unrankedTensors;

  /// MemRef type uniquing.
  using MemRefTypeSet = DenseSet<MemRefType *, MemRefTypeKeyInfo>;
  MemRefTypeSet memrefs;

  // Attribute uniquing.
  BoolAttributeStorage *boolAttrs[2] = {nullptr};
  DenseMap<int64_t, IntegerAttributeStorage *> integerAttrs;
  DenseSet<FloatAttributeStorage *, FloatAttrKeyInfo> floatAttrs;
  StringMap<StringAttributeStorage *> stringAttrs;
  using ArrayAttrSet = DenseSet<ArrayAttributeStorage *, ArrayAttrKeyInfo>;
  ArrayAttrSet arrayAttrs;
  DenseMap<AffineMap, AffineMapAttributeStorage *> affineMapAttrs;
  DenseMap<IntegerSet, IntegerSetAttributeStorage *> integerSetAttrs;
  DenseMap<Type *, TypeAttributeStorage *> typeAttrs;
  using AttributeListSet =
      DenseSet<AttributeListStorage *, AttributeListKeyInfo>;
  AttributeListSet attributeLists;
  DenseMap<const Function *, FunctionAttributeStorage *> functionAttrs;
  DenseMap<std::pair<VectorOrTensorType *, Attribute>,
           SplatElementsAttributeStorage *>
      splatElementsAttrs;
  using DenseElementsAttrSet =
      DenseSet<DenseElementsAttributeStorage *, DenseElementsAttrInfo>;
  DenseElementsAttrSet denseElementsAttrs;
  using OpaqueElementsAttrSet =
      DenseSet<OpaqueElementsAttributeStorage *, OpaqueElementsAttrInfo>;
  OpaqueElementsAttrSet opaqueElementsAttrs;
  DenseMap<std::tuple<Type *, Attribute, Attribute>,
           SparseElementsAttributeStorage *>
      sparseElementsAttrs;

public:
  MLIRContextImpl() : filenames(locationAllocator), identifiers(allocator) {}

  /// Copy the specified array of elements into memory managed by our bump
  /// pointer allocator.  This assumes the elements are all PODs.
  template <typename T> ArrayRef<T> copyInto(ArrayRef<T> elements) {
    auto result = allocator.Allocate<T>(elements.size());
    std::uninitialized_copy(elements.begin(), elements.end(), result);
    return ArrayRef<T>(result, elements.size());
  }
};
} // end namespace mlir

MLIRContext::MLIRContext() : impl(new MLIRContextImpl()) {
  new BuiltinDialect(this);
  registerAllDialects(this);
}

MLIRContext::~MLIRContext() {}

//===----------------------------------------------------------------------===//
// Diagnostic Handlers
//===----------------------------------------------------------------------===//

/// Register an issue handler with this MLIR context.  The issue handler is
/// passed location information along with a message and a DiagnosticKind enum
/// value that indicates the type of the diagnostic (e.g., Warning, Error).
void MLIRContext::registerDiagnosticHandler(
    const DiagnosticHandlerTy &handler) {
  getImpl().diagnosticHandler = handler;
}

/// Return the current diagnostic handler, or null if none is present.
auto MLIRContext::getDiagnosticHandler() const -> DiagnosticHandlerTy {
  return getImpl().diagnosticHandler;
}

/// This emits a diagnostic using the registered issue handle if present, or
/// with the default behavior if not.  The MLIR compiler should not generally
/// interact with this, it should use methods on Operation instead.
void MLIRContext::emitDiagnostic(Location *location, const llvm::Twine &message,
                                 DiagnosticKind kind) const {
  // If we had a handler registered, emit the diagnostic using it.
  auto handler = getImpl().diagnosticHandler;
  if (handler && location)
    return handler(location, message.str(), kind);

  // The default behavior for notes and warnings is to ignore them.
  if (kind != DiagnosticKind::Error)
    return;

  auto &os = llvm::errs();

  if (auto fileLoc = dyn_cast<FileLineColLoc>(location))
    os << fileLoc->getFilename() << ':' << fileLoc->getLine() << ':'
       << fileLoc->getColumn() << ": ";

  os << "error: ";

  // The default behavior for errors is to emit them to stderr and exit.
  os << message.str() << '\n';
  os.flush();
  exit(1);
}

//===----------------------------------------------------------------------===//
// Dialect and Operation Registration
//===----------------------------------------------------------------------===//

/// Return information about all registered IR dialects.
std::vector<Dialect *> MLIRContext::getRegisteredDialects() const {
  std::vector<Dialect *> result;
  result.reserve(getImpl().dialects.size());
  for (auto &dialect : getImpl().dialects)
    result.push_back(dialect.get());
  return result;
}

/// Register this dialect object with the specified context.  The context
/// takes ownership of the heap allocated dialect.
void Dialect::registerDialect(MLIRContext *context) {
  context->getImpl().dialects.push_back(std::unique_ptr<Dialect>(this));
}

/// Return information about all registered operations.  This isn't very
/// efficient, typically you should ask the operations about their properties
/// directly.
std::vector<AbstractOperation *> MLIRContext::getRegisteredOperations() const {
  // We just have the operations in a non-deterministic hash table order.  Dump
  // into a temporary array, then sort it by operation name to get a stable
  // ordering.
  StringMap<AbstractOperation> &registeredOps = getImpl().registeredOperations;

  std::vector<std::pair<StringRef, AbstractOperation *>> opsToSort;
  opsToSort.reserve(registeredOps.size());
  for (auto &elt : registeredOps)
    opsToSort.push_back({elt.first(), &elt.second});

  llvm::array_pod_sort(opsToSort.begin(), opsToSort.end());

  std::vector<AbstractOperation *> result;
  result.reserve(opsToSort.size());
  for (auto &elt : opsToSort)
    result.push_back(elt.second);
  return result;
}

void Dialect::addOperation(AbstractOperation opInfo) {
  assert(opInfo.name.startswith(opPrefix) &&
         "op name doesn't start with prefix");
  assert(&opInfo.dialect == this && "Dialect object mismatch");

  auto &impl = context->getImpl();
  if (!impl.registeredOperations.insert({opInfo.name, opInfo}).second) {
    llvm::errs() << "error: ops named '" << opInfo.name
                 << "' is already registered.\n";
    abort();
  }
}

/// Look up the specified operation in the operation set and return a pointer
/// to it if present.  Otherwise, return a null pointer.
const AbstractOperation *AbstractOperation::lookup(StringRef opName,
                                                   MLIRContext *context) {
  auto &impl = context->getImpl();
  auto it = impl.registeredOperations.find(opName);
  if (it != impl.registeredOperations.end())
    return &it->second;
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Identifier uniquing
//===----------------------------------------------------------------------===//

/// Return an identifier for the specified string.
Identifier Identifier::get(StringRef str, const MLIRContext *context) {
  assert(!str.empty() && "Cannot create an empty identifier");
  assert(str.find('\0') == StringRef::npos &&
         "Cannot create an identifier with a nul character");

  auto &impl = context->getImpl();
  auto it = impl.identifiers.insert({str, char()}).first;
  return Identifier(it->getKeyData());
}

//===----------------------------------------------------------------------===//
// Location uniquing
//===----------------------------------------------------------------------===//

UnknownLoc *UnknownLoc::get(MLIRContext *context) {
  auto &impl = context->getImpl();
  if (auto *result = impl.theUnknownLoc)
    return result;

  impl.theUnknownLoc = impl.allocator.Allocate<UnknownLoc>();
  new (impl.theUnknownLoc) UnknownLoc();
  return impl.theUnknownLoc;
}

UniquedFilename UniquedFilename::get(StringRef filename, MLIRContext *context) {
  auto &impl = context->getImpl();
  auto it = impl.filenames.insert({filename, char()}).first;
  return UniquedFilename(it->getKeyData());
}

FileLineColLoc *FileLineColLoc::get(UniquedFilename filename, unsigned line,
                                    unsigned column, MLIRContext *context) {
  auto &impl = context->getImpl();
  auto &entry =
      impl.fileLineColLocs[std::make_tuple(filename.data(), line, column)];
  if (!entry) {
    entry = impl.allocator.Allocate<FileLineColLoc>();
    new (entry) FileLineColLoc(filename, line, column);
  }

  return entry;
}

//===----------------------------------------------------------------------===//
// Type uniquing
//===----------------------------------------------------------------------===//

IntegerType *IntegerType::get(unsigned width, MLIRContext *context) {
  auto &impl = context->getImpl();

  auto *&result = impl.integers[width];
  if (!result) {
    result = impl.allocator.Allocate<IntegerType>();
    new (result) IntegerType(width, context);
  }

  return result;
}

FloatType *FloatType::get(Kind kind, MLIRContext *context) {
  assert(kind >= Kind::FIRST_FLOATING_POINT_TYPE &&
         kind <= Kind::LAST_FLOATING_POINT_TYPE && "Not an FP type kind");
  auto &impl = context->getImpl();

  // We normally have these types.
  auto *&entry =
      impl.floatTypes[(int)kind - int(Kind::FIRST_FLOATING_POINT_TYPE)];
  if (entry)
    return entry;

  // On the first use, we allocate them into the bump pointer.
  auto *ptr = impl.allocator.Allocate<FloatType>();

  // Initialize the memory using placement new.
  new (ptr) FloatType(kind, context);

  // Cache and return it.
  return entry = ptr;
}

OtherType *OtherType::get(Kind kind, MLIRContext *context) {
  assert(kind >= Kind::FIRST_OTHER_TYPE && kind <= Kind::LAST_OTHER_TYPE &&
         "Not an 'other' type kind");
  auto &impl = context->getImpl();

  // We normally have these types.
  auto *&entry = impl.otherTypes[(int)kind - int(Kind::FIRST_OTHER_TYPE)];
  if (entry)
    return entry;

  // On the first use, we allocate them into the bump pointer.
  auto *ptr = impl.allocator.Allocate<OtherType>();

  // Initialize the memory using placement new.
  new (ptr) OtherType(kind, context);

  // Cache and return it.
  return entry = ptr;
}

FunctionType *FunctionType::get(ArrayRef<Type *> inputs,
                                ArrayRef<Type *> results,
                                MLIRContext *context) {
  auto &impl = context->getImpl();

  // Look to see if we already have this function type.
  FunctionTypeKeyInfo::KeyTy key(inputs, results);
  auto existing = impl.functions.insert_as(nullptr, key);

  // If we already have it, return that value.
  if (!existing.second)
    return *existing.first;

  // On the first use, we allocate them into the bump pointer.
  auto *result = impl.allocator.Allocate<FunctionType>();

  // Copy the inputs and results into the bump pointer.
  SmallVector<Type *, 16> types;
  types.reserve(inputs.size() + results.size());
  types.append(inputs.begin(), inputs.end());
  types.append(results.begin(), results.end());
  auto typesList = impl.copyInto(ArrayRef<Type *>(types));

  // Initialize the memory using placement new.
  new (result)
      FunctionType(typesList.data(), inputs.size(), results.size(), context);

  // Cache and return it.
  return *existing.first = result;
}

VectorType *VectorType::get(ArrayRef<int> shape, Type *elementType) {
  assert(!shape.empty() && "vector types must have at least one dimension");
  assert((isa<FloatType>(elementType) || isa<IntegerType>(elementType)) &&
         "vectors elements must be primitives");
  assert(!std::any_of(shape.begin(), shape.end(), [](int i) {
    return i < 0;
  }) && "vector types must have static shape");

  auto *context = elementType->getContext();
  auto &impl = context->getImpl();

  // Look to see if we already have this vector type.
  VectorTypeKeyInfo::KeyTy key(elementType, shape);
  auto existing = impl.vectors.insert_as(nullptr, key);

  // If we already have it, return that value.
  if (!existing.second)
    return *existing.first;

  // On the first use, we allocate them into the bump pointer.
  auto *result = impl.allocator.Allocate<VectorType>();

  // Copy the shape into the bump pointer.
  shape = impl.copyInto(shape);

  // Initialize the memory using placement new.
  new (result) VectorType(shape, elementType, context);

  // Cache and return it.
  return *existing.first = result;
}

RankedTensorType *RankedTensorType::get(ArrayRef<int> shape,
                                        Type *elementType) {
  auto *context = elementType->getContext();
  auto &impl = context->getImpl();

  // Look to see if we already have this ranked tensor type.
  RankedTensorTypeKeyInfo::KeyTy key(elementType, shape);
  auto existing = impl.rankedTensors.insert_as(nullptr, key);

  // If we already have it, return that value.
  if (!existing.second)
    return *existing.first;

  // On the first use, we allocate them into the bump pointer.
  auto *result = impl.allocator.Allocate<RankedTensorType>();

  // Copy the shape into the bump pointer.
  shape = impl.copyInto(shape);

  // Initialize the memory using placement new.
  new (result) RankedTensorType(shape, elementType, context);

  // Cache and return it.
  return *existing.first = result;
}

UnrankedTensorType *UnrankedTensorType::get(Type *elementType) {
  auto *context = elementType->getContext();
  auto &impl = context->getImpl();

  // Look to see if we already have this unranked tensor type.
  auto *&result = impl.unrankedTensors[elementType];

  // If we already have it, return that value.
  if (result)
    return result;

  // On the first use, we allocate them into the bump pointer.
  result = impl.allocator.Allocate<UnrankedTensorType>();

  // Initialize the memory using placement new.
  new (result) UnrankedTensorType(elementType, context);
  return result;
}

MemRefType *MemRefType::get(ArrayRef<int> shape, Type *elementType,
                            ArrayRef<AffineMap> affineMapComposition,
                            unsigned memorySpace) {
  auto *context = elementType->getContext();
  auto &impl = context->getImpl();

  // Drop the unbounded identity maps from the composition.
  // This may lead to the composition becoming empty, which is interpreted as an
  // implicit identity.
  llvm::SmallVector<AffineMap, 2> cleanedAffineMapComposition;
  for (const auto &map : affineMapComposition) {
    if (map.isIdentity() && !map.isBounded())
      continue;
    cleanedAffineMapComposition.push_back(map);
  }
  affineMapComposition = cleanedAffineMapComposition;

  // Look to see if we already have this memref type.
  auto key =
      std::make_tuple(elementType, shape, affineMapComposition, memorySpace);
  auto existing = impl.memrefs.insert_as(nullptr, key);

  // If we already have it, return that value.
  if (!existing.second)
    return *existing.first;

  // On the first use, we allocate them into the bump pointer.
  auto *result = impl.allocator.Allocate<MemRefType>();

  // Copy the shape into the bump pointer.
  shape = impl.copyInto(shape);

  // Copy the affine map composition into the bump pointer.
  // TODO(andydavis) Assert that the structure of the composition is valid.
  affineMapComposition =
      impl.copyInto(ArrayRef<AffineMap>(affineMapComposition));

  // Initialize the memory using placement new.
  new (result) MemRefType(shape, elementType, affineMapComposition, memorySpace,
                          context);
  // Cache and return it.
  return *existing.first = result;
}

//===----------------------------------------------------------------------===//
// Attribute uniquing
//===----------------------------------------------------------------------===//

BoolAttr BoolAttr::get(bool value, MLIRContext *context) {
  auto *&result = context->getImpl().boolAttrs[value];
  if (result)
    return result;

  result = context->getImpl().allocator.Allocate<BoolAttributeStorage>();
  new (result) BoolAttributeStorage{{Attribute::Kind::Bool,
                                     /*isOrContainsFunction=*/false},
                                    value};
  return result;
}

IntegerAttr IntegerAttr::get(int64_t value, MLIRContext *context) {
  auto *&result = context->getImpl().integerAttrs[value];
  if (result)
    return result;

  result = context->getImpl().allocator.Allocate<IntegerAttributeStorage>();
  new (result) IntegerAttributeStorage{{Attribute::Kind::Integer,
                                        /*isOrContainsFunction=*/false},
                                       value};
  result->value = value;
  return result;
}

FloatAttr FloatAttr::get(double value, MLIRContext *context) {
  return get(APFloat(value), context);
}

FloatAttr FloatAttr::get(const APFloat &value, MLIRContext *context) {
  auto &impl = context->getImpl();

  // Look to see if the float attribute has been created already.
  auto existing = impl.floatAttrs.insert_as(nullptr, value);

  // If it has been created, return it.
  if (!existing.second)
    return *existing.first;

  // If it doesn't, create one, unique it and return it.
  const auto &apint = value.bitcastToAPInt();
  // Here one word's bitwidth equals to that of uint64_t.
  auto elements = ArrayRef<uint64_t>(apint.getRawData(), apint.getNumWords());

  auto byteSize =
      FloatAttributeStorage::totalSizeToAlloc<uint64_t>(elements.size());
  auto rawMem =
      impl.allocator.Allocate(byteSize, alignof(FloatAttributeStorage));
  auto result = ::new (rawMem) FloatAttributeStorage{
      {Attribute::Kind::Float, /*isOrContainsFunction=*/false},
      {},
      value.getSemantics(),
      elements.size()};
  std::uninitialized_copy(elements.begin(), elements.end(),
                          result->getTrailingObjects<uint64_t>());
  return *existing.first = result;
}

StringAttr StringAttr::get(StringRef bytes, MLIRContext *context) {
  auto it = context->getImpl().stringAttrs.insert({bytes, nullptr}).first;

  if (it->second)
    return it->second;

  auto result = context->getImpl().allocator.Allocate<StringAttributeStorage>();
  new (result) StringAttributeStorage{{Attribute::Kind::String,
                                       /*isOrContainsFunction=*/false},
                                      it->first()};
  it->second = result;
  return result;
}

ArrayAttr ArrayAttr::get(ArrayRef<Attribute> value, MLIRContext *context) {
  auto &impl = context->getImpl();

  // Look to see if we already have this.
  auto existing = impl.arrayAttrs.insert_as(nullptr, value);

  // If we already have it, return that value.
  if (!existing.second)
    return *existing.first;

  // On the first use, we allocate them into the bump pointer.
  auto *result = impl.allocator.Allocate<ArrayAttributeStorage>();

  // Copy the elements into the bump pointer.
  value = impl.copyInto(value);

  // Check to see if any of the elements have a function attr.
  bool hasFunctionAttr = false;
  for (auto elt : value)
    if (elt.isOrContainsFunction()) {
      hasFunctionAttr = true;
      break;
    }

  // Initialize the memory using placement new.
  new (result)
      ArrayAttributeStorage{{Attribute::Kind::Array, hasFunctionAttr}, value};

  // Cache and return it.
  return *existing.first = result;
}

AffineMapAttr AffineMapAttr::get(AffineMap value) {
  auto *context = value.getResult(0).getContext();
  auto &result = context->getImpl().affineMapAttrs[value];
  if (result)
    return result;

  result = context->getImpl().allocator.Allocate<AffineMapAttributeStorage>();
  new (result) AffineMapAttributeStorage{{Attribute::Kind::AffineMap,
                                          /*isOrContainsFunction=*/false},
                                         value};
  return result;
}

IntegerSetAttr IntegerSetAttr::get(IntegerSet value) {
  auto *context = value.getConstraint(0).getContext();
  auto &result = context->getImpl().integerSetAttrs[value];
  if (result)
    return result;

  result = context->getImpl().allocator.Allocate<IntegerSetAttributeStorage>();
  new (result) IntegerSetAttributeStorage{{Attribute::Kind::IntegerSet,
                                           /*isOrContainsFunction=*/false},
                                          value};
  return result;
}

TypeAttr TypeAttr::get(Type *type, MLIRContext *context) {
  auto *&result = context->getImpl().typeAttrs[type];
  if (result)
    return result;

  result = context->getImpl().allocator.Allocate<TypeAttributeStorage>();
  new (result) TypeAttributeStorage{{Attribute::Kind::Type,
                                     /*isOrContainsFunction=*/false},
                                    type};
  return result;
}

FunctionAttr FunctionAttr::get(const Function *value, MLIRContext *context) {
  assert(value && "Cannot get FunctionAttr for a null function");

  auto *&result = context->getImpl().functionAttrs[value];
  if (result)
    return result;

  result = context->getImpl().allocator.Allocate<FunctionAttributeStorage>();
  new (result) FunctionAttributeStorage{{Attribute::Kind::Function,
                                         /*isOrContainsFunction=*/true},
                                        const_cast<Function *>(value)};
  return result;
}

/// This function is used by the internals of the Function class to null out
/// attributes refering to functions that are about to be deleted.
void FunctionAttr::dropFunctionReference(Function *value) {
  // Check to see if there was an attribute referring to this function.
  auto &functionAttrs = value->getContext()->getImpl().functionAttrs;

  // If not, then we're done.
  auto it = functionAttrs.find(value);
  if (it == functionAttrs.end())
    return;

  // If so, null out the function reference in the attribute (to avoid dangling
  // pointers) and remove the entry from the map so the map doesn't contain
  // dangling keys.
  it->second->value = nullptr;
  functionAttrs.erase(it);
}

/// Perform a three-way comparison between the names of the specified
/// NamedAttributes.
static int compareNamedAttributes(const NamedAttribute *lhs,
                                  const NamedAttribute *rhs) {
  return lhs->first.str().compare(rhs->first.str());
}

/// Given a list of NamedAttribute's, canonicalize the list (sorting
/// by name) and return the unique'd result.  Note that the empty list is
/// represented with a null pointer.
AttributeListStorage *AttributeListStorage::get(ArrayRef<NamedAttribute> attrs,
                                                MLIRContext *context) {
  // We need to sort the element list to canonicalize it, but we also don't want
  // to do a ton of work in the super common case where the element list is
  // already sorted.
  SmallVector<NamedAttribute, 8> storage;
  switch (attrs.size()) {
  case 0:
    // An empty list is represented with a null pointer.
    return nullptr;
  case 1:
    // A single element is already sorted.
    break;
  case 2:
    // Don't invoke a general sort for two element case.
    if (attrs[0].first.str() > attrs[1].first.str()) {
      storage.push_back(attrs[1]);
      storage.push_back(attrs[0]);
      attrs = storage;
    }
    break;
  default:
    // Check to see they are sorted already.
    bool isSorted = true;
    for (unsigned i = 0, e = attrs.size() - 1; i != e; ++i) {
      if (attrs[i].first.str() > attrs[i + 1].first.str()) {
        isSorted = false;
        break;
      }
    }
    // If not, do a general sort.
    if (!isSorted) {
      storage.append(attrs.begin(), attrs.end());
      llvm::array_pod_sort(storage.begin(), storage.end(),
                           compareNamedAttributes);
      attrs = storage;
    }
  }

  auto &impl = context->getImpl();

  // Look to see if we already have this.
  auto existing = impl.attributeLists.insert_as(nullptr, attrs);

  // If we already have it, return that value.
  if (!existing.second)
    return *existing.first;

  // Otherwise, allocate a new AttributeListStorage, unique it and return it.
  auto byteSize =
      AttributeListStorage::totalSizeToAlloc<NamedAttribute>(attrs.size());
  auto rawMem = impl.allocator.Allocate(byteSize, alignof(NamedAttribute));

  //  Placement initialize the AggregateSymbolicValue.
  auto result = ::new (rawMem) AttributeListStorage(attrs.size());
  std::uninitialized_copy(attrs.begin(), attrs.end(),
                          result->getTrailingObjects<NamedAttribute>());
  return *existing.first = result;
}

SplatElementsAttr SplatElementsAttr::get(VectorOrTensorType *type,
                                         Attribute elt) {
  auto &impl = type->getContext()->getImpl();

  // Look to see if we already have this.
  auto *&result = impl.splatElementsAttrs[{type, elt}];

  // If we already have it, return that value.
  if (result)
    return result;

  // Otherwise, allocate them into the bump pointer.
  result = impl.allocator.Allocate<SplatElementsAttributeStorage>();
  new (result) SplatElementsAttributeStorage{{{Attribute::Kind::SplatElements,
                                               /*isOrContainsFunction=*/false},
                                              type},
                                             elt};

  return result;
}

DenseElementsAttr DenseElementsAttr::get(VectorOrTensorType *type,
                                         ArrayRef<char> data) {
  auto bitsRequired = (long)type->getBitWidth() * type->getNumElements();
  (void)bitsRequired;
  assert((bitsRequired <= data.size() * 8L) &&
         "Input data bit size should be larger than that type requires");

  auto &impl = type->getContext()->getImpl();

  // Look to see if this constant is already defined.
  DenseElementsAttrInfo::KeyTy key({type, data});
  auto existing = impl.denseElementsAttrs.insert_as(nullptr, key);

  // If we already have it, return that value.
  if (!existing.second)
    return *existing.first;

  // Otherwise, allocate a new one, unique it and return it.
  auto *eltType = type->getElementType();
  switch (eltType->getKind()) {
  case Type::Kind::BF16:
  case Type::Kind::F16:
  case Type::Kind::F32:
  case Type::Kind::F64: {
    auto *result = impl.allocator.Allocate<DenseFPElementsAttributeStorage>();
    auto *copy = (char *)impl.allocator.Allocate(data.size(), 64);
    std::uninitialized_copy(data.begin(), data.end(), copy);
    new (result) DenseFPElementsAttributeStorage{
        {{{Attribute::Kind::DenseFPElements, /*isOrContainsFunction=*/false},
          type},
         {copy, data.size()}}};
    return *existing.first = result;
  }
  case Type::Kind::Integer: {
    auto width = ::cast<IntegerType>(eltType)->getWidth();
    auto *result = impl.allocator.Allocate<DenseIntElementsAttributeStorage>();
    auto *copy = (char *)impl.allocator.Allocate(data.size(), 64);
    std::uninitialized_copy(data.begin(), data.end(), copy);
    new (result) DenseIntElementsAttributeStorage{
        {{{Attribute::Kind::DenseIntElements, /*isOrContainsFunction=*/false},
          type},
         {copy, data.size()}},
        width};
    return *existing.first = result;
  }
  default:
    llvm_unreachable("unexpected element type");
  }
}

OpaqueElementsAttr OpaqueElementsAttr::get(VectorOrTensorType *type,
                                           StringRef bytes) {
  assert(isValidTensorElementType(type->getElementType()) &&
         "Input element type should be a valid tensor element type");

  auto &impl = type->getContext()->getImpl();

  // Look to see if this constant is already defined.
  OpaqueElementsAttrInfo::KeyTy key({type, bytes});
  auto existing = impl.opaqueElementsAttrs.insert_as(nullptr, key);

  // If we already have it, return that value.
  if (!existing.second)
    return *existing.first;

  // Otherwise, allocate a new one, unique it and return it.
  auto *result = impl.allocator.Allocate<OpaqueElementsAttributeStorage>();
  bytes = bytes.copy(impl.allocator);
  new (result) OpaqueElementsAttributeStorage{
      {{Attribute::Kind::OpaqueElements, /*isOrContainsFunction=*/false}, type},
      bytes};
  return *existing.first = result;
}

SparseElementsAttr SparseElementsAttr::get(VectorOrTensorType *type,
                                           DenseIntElementsAttr indices,
                                           DenseElementsAttr values) {
  auto &impl = type->getContext()->getImpl();

  // Look to see if we already have this.
  auto key = std::make_tuple(type, indices, values);
  auto *&result = impl.sparseElementsAttrs[key];

  // If we already have it, return that value.
  if (result)
    return result;

  // Otherwise, allocate them into the bump pointer.
  result = impl.allocator.Allocate<SparseElementsAttributeStorage>();
  new (result) SparseElementsAttributeStorage{{{Attribute::Kind::SparseElements,
                                                /*isOrContainsFunction=*/false},
                                               type},
                                              indices,
                                              values};

  return result;
}

//===----------------------------------------------------------------------===//
// AffineMap and AffineExpr uniquing
//===----------------------------------------------------------------------===//

AffineMap AffineMap::get(unsigned dimCount, unsigned symbolCount,
                         ArrayRef<AffineExpr> results,
                         ArrayRef<AffineExpr> rangeSizes) {
  // The number of results can't be zero.
  assert(!results.empty());

  assert(rangeSizes.empty() || results.size() == rangeSizes.size());

  auto &impl = results[0].getContext()->getImpl();

  // Check if we already have this affine map.
  auto key = std::make_tuple(dimCount, symbolCount, results, rangeSizes);
  auto existing = impl.affineMaps.insert_as(AffineMap(nullptr), key);

  // If we already have it, return that value.
  if (!existing.second)
    return *existing.first;

  // On the first use, we allocate them into the bump pointer.
  auto *res = impl.allocator.Allocate<detail::AffineMapStorage>();

  // Copy the results and range sizes into the bump pointer.
  results = impl.copyInto(results);
  rangeSizes = impl.copyInto(rangeSizes);

  // Initialize the memory using placement new.
  new (res)
      detail::AffineMapStorage{dimCount, symbolCount, results, rangeSizes};

  // Cache and return it.
  return *existing.first = AffineMap(res);
}

/// Simplify add expression. Return nullptr if it can't be simplified.
static AffineExpr simplifyAdd(AffineExpr lhs, AffineExpr rhs) {
  auto lhsConst = lhs.dyn_cast<AffineConstantExpr>();
  auto rhsConst = rhs.dyn_cast<AffineConstantExpr>();
  // Fold if both LHS, RHS are a constant.
  if (lhsConst && rhsConst)
    return getAffineConstantExpr(lhsConst.getValue() + rhsConst.getValue(),
                                 lhs.getContext());

  // Canonicalize so that only the RHS is a constant. (4 + d0 becomes d0 + 4).
  // If only one of them is a symbolic expressions, make it the RHS.
  if (lhs.isa<AffineConstantExpr>() ||
      (lhs.isSymbolicOrConstant() && !rhs.isSymbolicOrConstant())) {
    return rhs + lhs;
  }

  // At this point, if there was a constant, it would be on the right.

  // Addition with a zero is a noop, return the other input.
  if (rhsConst) {
    if (rhsConst.getValue() == 0)
      return lhs;
  }
  // Fold successive additions like (d0 + 2) + 3 into d0 + 5.
  auto lBin = lhs.dyn_cast<AffineBinaryOpExpr>();
  if (lBin && rhsConst && lBin.getKind() == AffineExprKind::Add) {
    if (auto lrhs = lBin.getRHS().dyn_cast<AffineConstantExpr>())
      return lBin.getLHS() + (lrhs.getValue() + rhsConst.getValue());
  }

  // When doing successive additions, bring constant to the right: turn (d0 + 2)
  // + d1 into (d0 + d1) + 2.
  if (lBin && lBin.getKind() == AffineExprKind::Add) {
    if (auto lrhs = lBin.getRHS().dyn_cast<AffineConstantExpr>()) {
      return lBin.getLHS() + rhs + lrhs;
    }
  }

  return nullptr;
}

/// Simplify a multiply expression. Return nullptr if it can't be simplified.
static AffineExpr simplifyMul(AffineExpr lhs, AffineExpr rhs) {
  auto lhsConst = lhs.dyn_cast<AffineConstantExpr>();
  auto rhsConst = rhs.dyn_cast<AffineConstantExpr>();

  if (lhsConst && rhsConst)
    return getAffineConstantExpr(lhsConst.getValue() * rhsConst.getValue(),
                                 lhs.getContext());

  assert(lhs.isSymbolicOrConstant() || rhs.isSymbolicOrConstant());

  // Canonicalize the mul expression so that the constant/symbolic term is the
  // RHS. If both the lhs and rhs are symbolic, swap them if the lhs is a
  // constant. (Note that a constant is trivially symbolic).
  if (!rhs.isSymbolicOrConstant() || lhs.isa<AffineConstantExpr>()) {
    // At least one of them has to be symbolic.
    return rhs * lhs;
  }

  // At this point, if there was a constant, it would be on the right.

  // Multiplication with a one is a noop, return the other input.
  if (rhsConst) {
    if (rhsConst.getValue() == 1)
      return lhs;
    // Multiplication with zero.
    if (rhsConst.getValue() == 0)
      return rhsConst;
  }

  // Fold successive multiplications: eg: (d0 * 2) * 3 into d0 * 6.
  auto lBin = lhs.dyn_cast<AffineBinaryOpExpr>();
  if (lBin && rhsConst && lBin.getKind() == AffineExprKind::Mul) {
    if (auto lrhs = lBin.getRHS().dyn_cast<AffineConstantExpr>())
      return lBin.getLHS() * (lrhs.getValue() * rhsConst.getValue());
  }

  // When doing successive multiplication, bring constant to the right: turn (d0
  // * 2) * d1 into (d0 * d1) * 2.
  if (lBin && lBin.getKind() == AffineExprKind::Mul) {
    if (auto lrhs = lBin.getRHS().dyn_cast<AffineConstantExpr>()) {
      return (lBin.getLHS() * rhs) * lrhs;
    }
  }

  return nullptr;
}

static AffineExpr simplifyFloorDiv(AffineExpr lhs, AffineExpr rhs) {
  auto lhsConst = lhs.dyn_cast<AffineConstantExpr>();
  auto rhsConst = rhs.dyn_cast<AffineConstantExpr>();

  if (!rhsConst || rhsConst.getValue() < 1)
    return nullptr;

  if (lhsConst)
    return getAffineConstantExpr(
        floorDiv(lhsConst.getValue(), rhsConst.getValue()), lhs.getContext());

  // Fold floordiv of a multiply with a constant that is a multiple of the
  // divisor. Eg: (i * 128) floordiv 64 = i * 2.
  if (rhsConst.getValue() == 1)
    return lhs;

  auto lBin = lhs.dyn_cast<AffineBinaryOpExpr>();
  if (lBin && lBin.getKind() == AffineExprKind::Mul) {
    if (auto lrhs = lBin.getRHS().dyn_cast<AffineConstantExpr>()) {
      // rhsConst is known to be positive if a constant.
      if (lrhs.getValue() % rhsConst.getValue() == 0)
        return lBin.getLHS() * (lrhs.getValue() / rhsConst.getValue());
    }
  }

  return nullptr;
}

static AffineExpr simplifyCeilDiv(AffineExpr lhs, AffineExpr rhs) {
  auto lhsConst = lhs.dyn_cast<AffineConstantExpr>();
  auto rhsConst = rhs.dyn_cast<AffineConstantExpr>();

  if (!rhsConst || rhsConst.getValue() < 1)
    return nullptr;

  if (lhsConst)
    return getAffineConstantExpr(
        ceilDiv(lhsConst.getValue(), rhsConst.getValue()), lhs.getContext());

  // Fold ceildiv of a multiply with a constant that is a multiple of the
  // divisor. Eg: (i * 128) ceildiv 64 = i * 2.
  if (rhsConst.getValue() == 1)
    return lhs;

  auto lBin = lhs.dyn_cast<AffineBinaryOpExpr>();
  if (lBin && lBin.getKind() == AffineExprKind::Mul) {
    if (auto lrhs = lBin.getRHS().dyn_cast<AffineConstantExpr>()) {
      // rhsConst is known to be positive if a constant.
      if (lrhs.getValue() % rhsConst.getValue() == 0)
        return lBin.getLHS() * (lrhs.getValue() / rhsConst.getValue());
    }
  }

  return nullptr;
}

static AffineExpr simplifyMod(AffineExpr lhs, AffineExpr rhs) {
  auto lhsConst = lhs.dyn_cast<AffineConstantExpr>();
  auto rhsConst = rhs.dyn_cast<AffineConstantExpr>();

  if (!rhsConst || rhsConst.getValue() < 1)
    return nullptr;

  if (lhsConst)
    return getAffineConstantExpr(mod(lhsConst.getValue(), rhsConst.getValue()),
                                 lhs.getContext());

  // Fold modulo of an expression that is known to be a multiple of a constant
  // to zero if that constant is a multiple of the modulo factor. Eg: (i * 128)
  // mod 64 is folded to 0, and less trivially, (i*(j*4*(k*32))) mod 128 = 0.
  if (lhs.getLargestKnownDivisor() % rhsConst.getValue() == 0)
    return getAffineConstantExpr(0, lhs.getContext());

  return nullptr;
  // TODO(bondhugula): In general, this can be simplified more by using the GCD
  // test, or in general using quantifier elimination (add two new variables q
  // and r, and eliminate all variables from the linear system other than r. All
  // of this can be done through mlir/Analysis/'s FlatAffineConstraints.
}

/// Return a binary affine op expression with the specified op type and
/// operands: if it doesn't exist, create it and store it; if it is already
/// present, return from the list. The stored expressions are unique: they are
/// constructed and stored in a simplified/canonicalized form. The result after
/// simplification could be any form of affine expression.
AffineExpr AffineBinaryOpExprStorage::get(AffineExprKind kind, AffineExpr lhs,
                                          AffineExpr rhs) {
  auto &impl = lhs.getContext()->getImpl();

  // Check if we already have this affine expression, and return it if we do.
  auto keyValue = std::make_tuple((unsigned)kind, lhs, rhs);
  auto cached = impl.affineExprs.find(keyValue);
  if (cached != impl.affineExprs.end())
    return cached->second;

  // Simplify the expression if possible.
  AffineExpr simplified;
  switch (kind) {
  case AffineExprKind::Add:
    simplified = simplifyAdd(lhs, rhs);
    break;
  case AffineExprKind::Mul:
    simplified = simplifyMul(lhs, rhs);
    break;
  case AffineExprKind::FloorDiv:
    simplified = simplifyFloorDiv(lhs, rhs);
    break;
  case AffineExprKind::CeilDiv:
    simplified = simplifyCeilDiv(lhs, rhs);
    break;
  case AffineExprKind::Mod:
    simplified = simplifyMod(lhs, rhs);
    break;
  default:
    llvm_unreachable("unexpected binary affine expr");
  }

  // The simplified one would have already been cached; just return it.
  if (simplified)
    return simplified;

  // An expression with these operands will already be in the
  // simplified/canonical form. Create and store it.
  auto *result = impl.allocator.Allocate<AffineBinaryOpExprStorage>();
  // Initialize the memory using placement new.
  new (result) AffineBinaryOpExprStorage{{kind, lhs.getContext()}, lhs, rhs};
  bool inserted = impl.affineExprs.insert({keyValue, result}).second;
  assert(inserted && "the expression shouldn't already exist in the map");
  (void)inserted;
  return result;
}

AffineExpr mlir::getAffineDimExpr(unsigned position, MLIRContext *context) {
  auto &impl = context->getImpl();

  // Check if we need to resize.
  if (position >= impl.dimExprs.size())
    impl.dimExprs.resize(position + 1, nullptr);

  auto *&result = impl.dimExprs[position];
  if (result)
    return result;

  result = impl.allocator.Allocate<AffineDimExprStorage>();
  // Initialize the memory using placement new.
  new (result) AffineDimExprStorage{{AffineExprKind::DimId, context}, position};
  return result;
}

AffineExpr mlir::getAffineSymbolExpr(unsigned position, MLIRContext *context) {
  auto &impl = context->getImpl();

  // Check if we need to resize.
  if (position >= impl.symbolExprs.size())
    impl.symbolExprs.resize(position + 1, nullptr);

  auto *&result = impl.symbolExprs[position];
  if (result)
    return result;

  result = impl.allocator.Allocate<AffineSymbolExprStorage>();
  // Initialize the memory using placement new.
  new (result)
      AffineSymbolExprStorage{{AffineExprKind::SymbolId, context}, position};
  return result;
}

AffineExpr mlir::getAffineConstantExpr(int64_t constant, MLIRContext *context) {
  auto &impl = context->getImpl();
  auto *&result = impl.constExprs[constant];

  if (result)
    return result;

  result = impl.allocator.Allocate<AffineConstantExprStorage>();
  // Initialize the memory using placement new.
  new (result)
      AffineConstantExprStorage{{AffineExprKind::Constant, context}, constant};
  return result;
}

//===----------------------------------------------------------------------===//
// Integer Sets: these are allocated into the bump pointer, and are immutable.
// Unlike AffineMap's, these are uniqued only if they are small.
//===----------------------------------------------------------------------===//

IntegerSet IntegerSet::get(unsigned dimCount, unsigned symbolCount,
                           ArrayRef<AffineExpr> constraints,
                           ArrayRef<bool> eqFlags) {
  // The number of constraints can't be zero.
  assert(!constraints.empty());
  assert(constraints.size() == eqFlags.size());

  bool unique = constraints.size() < IntegerSet::kUniquingThreshold;

  auto &impl = constraints[0].getContext()->getImpl();

  std::pair<DenseSet<IntegerSet, IntegerSetKeyInfo>::Iterator, bool> existing;
  if (unique) {
    // Check if we already have this integer set.
    auto key = std::make_tuple(dimCount, symbolCount, constraints, eqFlags);
    existing = impl.integerSets.insert_as(IntegerSet(nullptr), key);

    // If we already have it, return that value.
    if (!existing.second)
      return *existing.first;
  }

  // On the first use, we allocate them into the bump pointer.
  auto *res = impl.allocator.Allocate<detail::IntegerSetStorage>();

  // Copy the results and equality flags into the bump pointer.
  constraints = impl.copyInto(constraints);
  eqFlags = impl.copyInto(eqFlags);

  // Initialize the memory using placement new.
  new (res)
      detail::IntegerSetStorage{dimCount, symbolCount, constraints, eqFlags};

  if (unique)
    // Cache and return it.
    return *existing.first = IntegerSet(res);

  return IntegerSet(res);
}
