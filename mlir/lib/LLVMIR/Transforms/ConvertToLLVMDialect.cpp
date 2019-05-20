//===- ConvertToLLVMDialect.cpp - MLIR to LLVM dialect conversion ---------===//
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
// This file implements a pass to convert MLIR standard and builtin dialects
// into the LLVM IR dialect.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/Builders.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/LLVMIR/LLVMDialect.h"
#include "mlir/LLVMIR/LLVMLowering.h"
#include "mlir/LLVMIR/Transforms.h"
#include "mlir/Pass/Pass.h"
#include "mlir/StandardOps/Ops.h"
#include "mlir/Support/Functional.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Transforms/Utils.h"

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"

using namespace mlir;

// Get the LLVM context.
llvm::LLVMContext &LLVMLowering::getLLVMContext() {
  return module->getContext();
}

// Wrap the given LLVM IR type into an LLVM IR dialect type.
Type LLVMLowering::wrap(llvm::Type *llvmType) {
  return LLVM::LLVMType::get(llvmDialect->getContext(), llvmType);
}

// Extract an LLVM IR type from the LLVM IR dialect type.
llvm::Type *LLVMLowering::unwrap(Type type) {
  if (!type)
    return nullptr;
  auto *mlirContext = type.getContext();
  auto wrappedLLVMType = type.dyn_cast<LLVM::LLVMType>();
  if (!wrappedLLVMType)
    return mlirContext->emitError(UnknownLoc::get(mlirContext),
                                  "conversion resulted in a non-LLVM type"),
           nullptr;
  return wrappedLLVMType.getUnderlyingType();
}

llvm::IntegerType *LLVMLowering::getIndexType() {
  return llvm::IntegerType::get(llvmDialect->getLLVMContext(),
                                module->getDataLayout().getPointerSizeInBits());
}

Type LLVMLowering::convertIndexType(IndexType type) {
  return wrap(getIndexType());
}

Type LLVMLowering::convertIntegerType(IntegerType type) {
  return wrap(
      llvm::Type::getIntNTy(llvmDialect->getLLVMContext(), type.getWidth()));
}

Type LLVMLowering::convertFloatType(FloatType type) {
  switch (type.getKind()) {
  case mlir::StandardTypes::F32:
    return wrap(llvm::Type::getFloatTy(llvmDialect->getLLVMContext()));
  case mlir::StandardTypes::F64:
    return wrap(llvm::Type::getDoubleTy(llvmDialect->getLLVMContext()));
  case mlir::StandardTypes::F16:
    return wrap(llvm::Type::getHalfTy(llvmDialect->getLLVMContext()));
  case mlir::StandardTypes::BF16: {
    auto *mlirContext = llvmDialect->getContext();
    return mlirContext->emitError(UnknownLoc::get(mlirContext),
                                  "unsupported type: BF16"),
           Type();
  }
  default:
    llvm_unreachable("non-float type in convertFloatType");
  }
}

// Function types are converted to LLVM Function types by recursively converting
// argument and result types.  If MLIR Function has zero results, the LLVM
// Function has one VoidType result.  If MLIR Function has more than one result,
// they are into an LLVM StructType in their order of appearance.
Type LLVMLowering::convertFunctionType(FunctionType type) {
  // Convert argument types one by one and check for errors.
  SmallVector<llvm::Type *, 8> argTypes;
  for (auto t : type.getInputs()) {
    auto converted = convertType(t);
    if (!converted)
      return {};
    argTypes.push_back(unwrap(converted));
  }

  // If function does not return anything, create the void result type,
  // if it returns on element, convert it, otherwise pack the result types into
  // a struct.
  llvm::Type *resultType =
      type.getNumResults() == 0
          ? llvm::Type::getVoidTy(llvmDialect->getLLVMContext())
          : unwrap(packFunctionResults(type.getResults()));
  if (!resultType)
    return {};
  return wrap(llvm::FunctionType::get(resultType, argTypes, /*isVarArg=*/false)
                  ->getPointerTo());
}

// Convert a MemRef to an LLVM type. If the memref is statically-shaped, then
// we return a pointer to the converted element type. Otherwise we return an
// LLVM stucture type, where the first element of the structure type is a
// pointer to the elemental type of the MemRef and the following N elements are
// values of the Index type, one for each of N dynamic dimensions of the MemRef.
Type LLVMLowering::convertMemRefType(MemRefType type) {
  llvm::Type *elementType = unwrap(convertType(type.getElementType()));
  if (!elementType)
    return {};
  auto ptrType = elementType->getPointerTo();

  // Extra value for the memory space.
  unsigned numDynamicSizes = type.getNumDynamicDims();
  // If memref is statically-shaped we return the underlying pointer type.
  if (numDynamicSizes == 0) {
    return wrap(ptrType);
  }
  SmallVector<llvm::Type *, 8> types(numDynamicSizes + 1, getIndexType());
  types.front() = ptrType;

  return wrap(llvm::StructType::get(llvmDialect->getLLVMContext(), types));
}

// Convert a 1D vector type to an LLVM vector type.
Type LLVMLowering::convertVectorType(VectorType type) {
  if (type.getRank() != 1) {
    auto *mlirContext = llvmDialect->getContext();
    mlirContext->emitError(UnknownLoc::get(mlirContext),
                           "only 1D vectors are supported");
    return {};
  }

  llvm::Type *elementType = unwrap(convertType(type.getElementType()));
  return elementType
             ? wrap(llvm::VectorType::get(elementType, type.getShape().front()))
             : Type();
}

// Dispatch based on the actual type.  Return null type on error.
Type LLVMLowering::convertStandardType(Type type) {
  if (auto funcType = type.dyn_cast<FunctionType>())
    return convertFunctionType(funcType);
  if (auto intType = type.dyn_cast<IntegerType>())
    return convertIntegerType(intType);
  if (auto floatType = type.dyn_cast<FloatType>())
    return convertFloatType(floatType);
  if (auto indexType = type.dyn_cast<IndexType>())
    return convertIndexType(indexType);
  if (auto memRefType = type.dyn_cast<MemRefType>())
    return convertMemRefType(memRefType);
  if (auto vectorType = type.dyn_cast<VectorType>())
    return convertVectorType(vectorType);
  if (auto llvmType = type.dyn_cast<LLVM::LLVMType>())
    return llvmType;

  return {};
}

// Convert the element type of the memref `t` to to an LLVM type using
// `lowering`, get a pointer LLVM type pointing to the converted `t`, wrap it
// into the MLIR LLVM dialect type and return.
static Type getMemRefElementPtrType(MemRefType t, LLVMLowering &lowering) {
  auto elementType = t.getElementType();
  auto converted = lowering.convertType(elementType);
  if (!converted)
    return {};
  llvm::Type *llvmType = converted.cast<LLVM::LLVMType>().getUnderlyingType();
  return LLVM::LLVMType::get(t.getContext(), llvmType->getPointerTo());
}

LLVMOpLowering::LLVMOpLowering(StringRef rootOpName, MLIRContext *context,
                               LLVMLowering &lowering_)
    : DialectOpConversion(rootOpName, /*benefit=*/1, context),
      lowering(lowering_) {}

namespace {
// Base class for Standard to LLVM IR op conversions.  Matches the Op type
// provided as template argument.  Carries a reference to the LLVM dialect in
// case it is necessary for rewriters.
template <typename SourceOp>
class LLVMLegalizationPattern : public LLVMOpLowering {
public:
  // Construct a conversion pattern.
  explicit LLVMLegalizationPattern(LLVM::LLVMDialect &dialect_,
                                   LLVMLowering &lowering_)
      : LLVMOpLowering(SourceOp::getOperationName(), dialect_.getContext(),
                       lowering_),
        dialect(dialect_) {}

  PatternMatchResult match(Operation *op) const override {
    return this->matchSuccess();
  }

  // Get the LLVM IR dialect.
  LLVM::LLVMDialect &getDialect() const { return dialect; }
  // Get the LLVM context.
  llvm::LLVMContext &getContext() const { return dialect.getLLVMContext(); }
  // Get the LLVM module in which the types are constructed.
  llvm::Module &getModule() const { return dialect.getLLVMModule(); }

  // Get the MLIR type wrapping the LLVM integer type whose bit width is defined
  // by the pointer size used in the LLVM module.
  LLVM::LLVMType getIndexType() const {
    llvm::Type *llvmType = llvm::Type::getIntNTy(
        getContext(), getModule().getDataLayout().getPointerSizeInBits());
    return LLVM::LLVMType::get(dialect.getContext(), llvmType);
  }

  // Get the MLIR type wrapping the LLVM i8* type.
  LLVM::LLVMType getVoidPtrType() const {
    return LLVM::LLVMType::get(dialect.getContext(),
                               llvm::Type::getInt8PtrTy(getContext()));
  }

  // Create an LLVM IR pseudo-operation defining the given index constant.
  Value *createIndexConstant(PatternRewriter &builder, Location loc,
                             uint64_t value) const {
    auto attr = builder.getIntegerAttr(builder.getIndexType(), value);
    return builder.create<LLVM::ConstantOp>(loc, getIndexType(), attr);
  }

  // Get the array attribute named "position" containing the given list of
  // integers as integer attribute elements.
  static ArrayAttr getIntegerArrayAttr(PatternRewriter &builder,
                                       ArrayRef<int64_t> values) {
    SmallVector<Attribute, 4> attrs;
    attrs.reserve(values.size());
    for (int64_t pos : values)
      attrs.push_back(builder.getIntegerAttr(builder.getIndexType(), pos));
    return builder.getArrayAttr(attrs);
  }

  // Extract raw data pointer value from a value representing a memref.
  static Value *extractMemRefElementPtr(PatternRewriter &builder, Location loc,
                                        Value *convertedMemRefValue,
                                        Type elementTypePtr,
                                        bool hasStaticShape) {
    Value *buffer;
    if (hasStaticShape)
      return convertedMemRefValue;
    else
      return builder.create<LLVM::ExtractValueOp>(
          loc, elementTypePtr, convertedMemRefValue,
          getIntegerArrayAttr(builder, 0));
    return buffer;
  }

protected:
  LLVM::LLVMDialect &dialect;
};

// Given a range of MLIR typed objects, return a list of their types.
template <typename T>
SmallVector<Type, 4> getTypes(llvm::iterator_range<T> range) {
  SmallVector<Type, 4> types;
  types.reserve(llvm::size(range));
  for (auto operand : range) {
    types.push_back(operand->getType());
  }
  return types;
}

// Basic lowering implementation for one-to-one rewriting from Standard Ops to
// LLVM Dialect Ops.
template <typename SourceOp, typename TargetOp>
struct OneToOneLLVMOpLowering : public LLVMLegalizationPattern<SourceOp> {
  using LLVMLegalizationPattern<SourceOp>::LLVMLegalizationPattern;
  using Super = OneToOneLLVMOpLowering<SourceOp, TargetOp>;

  // Convert the type of the result to an LLVM type, pass operands as is,
  // preserve attributes.
  void rewrite(Operation *op, ArrayRef<Value *> operands,
               PatternRewriter &rewriter) const override {
    unsigned numResults = op->getNumResults();

    Type packedType;
    if (numResults != 0) {
      packedType =
          this->lowering.packFunctionResults(getTypes(op->getResults()));
      assert(packedType && "type conversion failed, such operation should not "
                           "have been matched");
    }

    auto newOp = rewriter.create<TargetOp>(op->getLoc(), packedType, operands,
                                           op->getAttrs());

    // If the operation produced 0 or 1 result, return them immediately.
    if (numResults == 0)
      return;
    if (numResults == 1)
      return rewriter.replaceOp(op, newOp.getOperation()->getResult(0));

    // Otherwise, it had been converted to an operation producing a structure.
    // Extract individual results from the structure and return them as list.
    SmallVector<Value *, 4> results;
    results.reserve(numResults);
    for (unsigned i = 0; i < numResults; ++i) {
      auto type = this->lowering.convertType(op->getResult(i)->getType());
      results.push_back(rewriter.create<LLVM::ExtractValueOp>(
          op->getLoc(), type, newOp.getOperation()->getResult(0),
          this->getIntegerArrayAttr(rewriter, i)));
    }
    rewriter.replaceOp(op, results);
  }
};

// Specific lowerings.
// FIXME: this should be tablegen'ed.
struct AddIOpLowering : public OneToOneLLVMOpLowering<AddIOp, LLVM::AddOp> {
  using Super::Super;
};
struct SubIOpLowering : public OneToOneLLVMOpLowering<SubIOp, LLVM::SubOp> {
  using Super::Super;
};
struct MulIOpLowering : public OneToOneLLVMOpLowering<MulIOp, LLVM::MulOp> {
  using Super::Super;
};
struct DivISOpLowering : public OneToOneLLVMOpLowering<DivISOp, LLVM::SDivOp> {
  using Super::Super;
};
struct DivIUOpLowering : public OneToOneLLVMOpLowering<DivIUOp, LLVM::UDivOp> {
  using Super::Super;
};
struct RemISOpLowering : public OneToOneLLVMOpLowering<RemISOp, LLVM::SRemOp> {
  using Super::Super;
};
struct RemIUOpLowering : public OneToOneLLVMOpLowering<RemIUOp, LLVM::URemOp> {
  using Super::Super;
};
struct AndOpLowering : public OneToOneLLVMOpLowering<AndOp, LLVM::AndOp> {
  using Super::Super;
};
struct OrOpLowering : public OneToOneLLVMOpLowering<OrOp, LLVM::OrOp> {
  using Super::Super;
};
struct XOrOpLowering : public OneToOneLLVMOpLowering<XOrOp, LLVM::XOrOp> {
  using Super::Super;
};
struct AddFOpLowering : public OneToOneLLVMOpLowering<AddFOp, LLVM::FAddOp> {
  using Super::Super;
};
struct SubFOpLowering : public OneToOneLLVMOpLowering<SubFOp, LLVM::FSubOp> {
  using Super::Super;
};
struct MulFOpLowering : public OneToOneLLVMOpLowering<MulFOp, LLVM::FMulOp> {
  using Super::Super;
};
struct DivFOpLowering : public OneToOneLLVMOpLowering<DivFOp, LLVM::FDivOp> {
  using Super::Super;
};
struct RemFOpLowering : public OneToOneLLVMOpLowering<RemFOp, LLVM::FRemOp> {
  using Super::Super;
};
struct CmpIOpLowering : public OneToOneLLVMOpLowering<CmpIOp, LLVM::ICmpOp> {
  using Super::Super;
};
struct SelectOpLowering
    : public OneToOneLLVMOpLowering<SelectOp, LLVM::SelectOp> {
  using Super::Super;
};
struct CallOpLowering : public OneToOneLLVMOpLowering<CallOp, LLVM::CallOp> {
  using Super::Super;
};
struct CallIndirectOpLowering
    : public OneToOneLLVMOpLowering<CallIndirectOp, LLVM::CallOp> {
  using Super::Super;
};
struct ConstLLVMOpLowering
    : public OneToOneLLVMOpLowering<ConstantOp, LLVM::ConstantOp> {
  using Super::Super;
};

// Check if the MemRefType `type` is supported by the lowering. We currently do
// not support memrefs with affine maps and non-default memory spaces.
static bool isSupportedMemRefType(MemRefType type) {
  if (!type.getAffineMaps().empty())
    return false;
  if (type.getMemorySpace() != 0)
    return false;
  return true;
}

// An `alloc` is converted into a definition of a memref descriptor value and
// a call to `malloc` to allocate the underlying data buffer.  The memref
// descriptor is of the LLVM structure type where the first element is a pointer
// to the (typed) data buffer, and the remaining elements serve to store
// dynamic sizes of the memref using LLVM-converted `index` type.
struct AllocOpLowering : public LLVMLegalizationPattern<AllocOp> {
  using LLVMLegalizationPattern<AllocOp>::LLVMLegalizationPattern;

  PatternMatchResult match(Operation *op) const override {
    MemRefType type = cast<AllocOp>(op).getType();
    return isSupportedMemRefType(type) ? matchSuccess() : matchFailure();
  }

  void rewrite(Operation *op, ArrayRef<Value *> operands,
               PatternRewriter &rewriter) const override {
    auto allocOp = cast<AllocOp>(op);
    MemRefType type = allocOp.getType();

    // Get actual sizes of the memref as values: static sizes are constant
    // values and dynamic sizes are passed to 'alloc' as operands.  In case of
    // zero-dimensional memref, assume a scalar (size 1).
    SmallVector<Value *, 4> sizes;
    auto numOperands = allocOp.getNumOperands();
    sizes.reserve(numOperands);
    unsigned i = 0;
    for (int64_t s : type.getShape())
      sizes.push_back(s == -1 ? operands[i++]
                              : createIndexConstant(rewriter, op->getLoc(), s));
    if (sizes.empty())
      sizes.push_back(createIndexConstant(rewriter, op->getLoc(), 1));

    // Compute the total number of memref elements.
    Value *cumulativeSize = sizes.front();
    for (unsigned i = 1, e = sizes.size(); i < e; ++i)
      cumulativeSize = rewriter.create<LLVM::MulOp>(
          op->getLoc(), getIndexType(),
          ArrayRef<Value *>{cumulativeSize, sizes[i]});

    // Compute the total amount of bytes to allocate.
    auto elementType = type.getElementType();
    assert((elementType.isIntOrFloat() || elementType.isa<VectorType>()) &&
           "invalid memref element type");
    uint64_t elementSize = 0;
    if (auto vectorType = elementType.dyn_cast<VectorType>())
      elementSize = vectorType.getNumElements() *
                    llvm::divideCeil(vectorType.getElementTypeBitWidth(), 8);
    else
      elementSize = llvm::divideCeil(elementType.getIntOrFloatBitWidth(), 8);
    cumulativeSize = rewriter.create<LLVM::MulOp>(
        op->getLoc(), getIndexType(),
        ArrayRef<Value *>{
            cumulativeSize,
            createIndexConstant(rewriter, op->getLoc(), elementSize)});

    // Insert the `malloc` declaration if it is not already present.
    Function *mallocFunc =
        op->getFunction()->getModule()->getNamedFunction("malloc");
    if (!mallocFunc) {
      auto mallocType =
          rewriter.getFunctionType(getIndexType(), getVoidPtrType());
      mallocFunc = new Function(rewriter.getUnknownLoc(), "malloc", mallocType);
      op->getFunction()->getModule()->getFunctions().push_back(mallocFunc);
    }

    // Allocate the underlying buffer and store a pointer to it in the MemRef
    // descriptor.
    Value *allocated =
        rewriter
            .create<LLVM::CallOp>(op->getLoc(), getVoidPtrType(),
                                  rewriter.getFunctionAttr(mallocFunc),
                                  cumulativeSize)
            .getResult(0);
    auto structElementType = lowering.convertType(elementType);
    auto elementPtrType = LLVM::LLVMType::get(
        op->getContext(), structElementType.cast<LLVM::LLVMType>()
                              .getUnderlyingType()
                              ->getPointerTo());
    allocated = rewriter.create<LLVM::BitcastOp>(op->getLoc(), elementPtrType,
                                                 ArrayRef<Value *>(allocated));

    // Deal with static memrefs
    if (numOperands == 0)
      return rewriter.replaceOp(op, allocated);

    // Create the MemRef descriptor.
    auto structType = lowering.convertType(type);
    Value *memRefDescriptor = rewriter.create<LLVM::UndefOp>(
        op->getLoc(), structType, ArrayRef<Value *>{});

    memRefDescriptor = rewriter.create<LLVM::InsertValueOp>(
        op->getLoc(), structType, memRefDescriptor, allocated,
        getIntegerArrayAttr(rewriter, 0));

    // Store dynamically allocated sizes in the descriptor.  Dynamic sizes are
    // passed in as operands.
    for (auto indexedSize : llvm::enumerate(operands)) {
      memRefDescriptor = rewriter.create<LLVM::InsertValueOp>(
          op->getLoc(), structType, memRefDescriptor, indexedSize.value(),
          getIntegerArrayAttr(rewriter, 1 + indexedSize.index()));
    }

    // Return the final value of the descriptor.
    rewriter.replaceOp(op, memRefDescriptor);
  }
};

// A `dealloc` is converted into a call to `free` on the underlying data buffer.
// The memref descriptor being an SSA value, there is no need to clean it up
// in any way.
struct DeallocOpLowering : public LLVMLegalizationPattern<DeallocOp> {
  using LLVMLegalizationPattern<DeallocOp>::LLVMLegalizationPattern;

  void rewrite(Operation *op, ArrayRef<Value *> operands,
               PatternRewriter &rewriter) const override {
    assert(operands.size() == 1 && "dealloc takes one operand");

    // Insert the `free` declaration if it is not already present.
    Function *freeFunc =
        op->getFunction()->getModule()->getNamedFunction("free");
    if (!freeFunc) {
      auto freeType = rewriter.getFunctionType(getVoidPtrType(), {});
      freeFunc = new Function(rewriter.getUnknownLoc(), "free", freeType);
      op->getFunction()->getModule()->getFunctions().push_back(freeFunc);
    }

    auto *type =
        operands[0]->getType().cast<LLVM::LLVMType>().getUnderlyingType();
    auto hasStaticShape = type->isPointerTy();
    Type elementPtrType =
        (hasStaticShape)
            ? rewriter.getType<LLVM::LLVMType>(type)
            : rewriter.getType<LLVM::LLVMType>(
                  cast<llvm::StructType>(type)->getStructElementType(0));
    Value *bufferPtr = extractMemRefElementPtr(
        rewriter, op->getLoc(), operands[0], elementPtrType, hasStaticShape);
    Value *casted = rewriter.create<LLVM::BitcastOp>(
        op->getLoc(), getVoidPtrType(), bufferPtr);
    rewriter.create<LLVM::CallOp>(op->getLoc(), ArrayRef<Type>(),
                                  rewriter.getFunctionAttr(freeFunc), casted);
  }
};

struct MemRefCastOpLowering : public LLVMLegalizationPattern<MemRefCastOp> {
  using LLVMLegalizationPattern<MemRefCastOp>::LLVMLegalizationPattern;

  PatternMatchResult match(Operation *op) const override {
    auto memRefCastOp = cast<MemRefCastOp>(op);
    MemRefType sourceType =
        memRefCastOp.getOperand()->getType().cast<MemRefType>();
    MemRefType targetType = memRefCastOp.getType();
    return (isSupportedMemRefType(targetType) &&
            isSupportedMemRefType(sourceType))
               ? matchSuccess()
               : matchFailure();
  }

  void rewrite(Operation *op, ArrayRef<Value *> operands,
               PatternRewriter &rewriter) const override {
    auto memRefCastOp = cast<MemRefCastOp>(op);
    auto targetType = memRefCastOp.getType();
    auto sourceType = memRefCastOp.getOperand()->getType().cast<MemRefType>();

    // Copy the data buffer pointer.
    auto elementTypePtr = getMemRefElementPtrType(targetType, lowering);
    Value *buffer =
        extractMemRefElementPtr(rewriter, op->getLoc(), operands[0],
                                elementTypePtr, sourceType.hasStaticShape());
    // Account for static memrefs as target types
    if (targetType.hasStaticShape())
      return rewriter.replaceOp(op, buffer);

    // Create the new MemRef descriptor.
    auto structType = lowering.convertType(targetType);
    Value *newDescriptor = rewriter.create<LLVM::UndefOp>(
        op->getLoc(), structType, ArrayRef<Value *>{});
    // Otherwise target type is dynamic memref, so create a proper descriptor.
    newDescriptor = rewriter.create<LLVM::InsertValueOp>(
        op->getLoc(), structType, newDescriptor, buffer,
        getIntegerArrayAttr(rewriter, 0));

    // Fill in the dynamic sizes of the new descriptor.  If the size was
    // dynamic, copy it from the old descriptor.  If the size was static, insert
    // the constant.  Note that the positions of dynamic sizes in the
    // descriptors start from 1 (the buffer pointer is at position zero).
    int64_t sourceDynamicDimIdx = 1;
    int64_t targetDynamicDimIdx = 1;
    for (int i = 0, e = sourceType.getRank(); i < e; ++i) {
      // Ignore new static sizes (they will be known from the type).  If the
      // size was dynamic, update the index of dynamic types.
      if (targetType.getShape()[i] != -1) {
        if (sourceType.getShape()[i] == -1)
          ++sourceDynamicDimIdx;
        continue;
      }

      auto sourceSize = sourceType.getShape()[i];
      Value *size =
          sourceSize == -1
              ? rewriter.create<LLVM::ExtractValueOp>(
                    op->getLoc(), getIndexType(),
                    operands[0], // NB: dynamic memref
                    getIntegerArrayAttr(rewriter, sourceDynamicDimIdx++))
              : createIndexConstant(rewriter, op->getLoc(), sourceSize);
      newDescriptor = rewriter.create<LLVM::InsertValueOp>(
          op->getLoc(), structType, newDescriptor, size,
          getIntegerArrayAttr(rewriter, targetDynamicDimIdx++));
    }
    assert(sourceDynamicDimIdx - 1 == sourceType.getNumDynamicDims() &&
           "source dynamic dimensions were not processed");
    assert(targetDynamicDimIdx - 1 == targetType.getNumDynamicDims() &&
           "target dynamic dimensions were not set up");

    rewriter.replaceOp(op, newDescriptor);
  }
};

// A `dim` is converted to a constant for static sizes and to an access to the
// size stored in the memref descriptor for dynamic sizes.
struct DimOpLowering : public LLVMLegalizationPattern<DimOp> {
  using LLVMLegalizationPattern<DimOp>::LLVMLegalizationPattern;

  PatternMatchResult match(Operation *op) const override {
    auto dimOp = cast<DimOp>(op);
    MemRefType type = dimOp.getOperand()->getType().cast<MemRefType>();
    return isSupportedMemRefType(type) ? matchSuccess() : matchFailure();
  }

  void rewrite(Operation *op, ArrayRef<Value *> operands,
               PatternRewriter &rewriter) const override {
    assert(operands.size() == 1 && "expected exactly one operand");
    auto dimOp = cast<DimOp>(op);
    MemRefType type = dimOp.getOperand()->getType().cast<MemRefType>();

    auto shape = type.getShape();
    uint64_t index = dimOp.getIndex();
    // Extract dynamic size from the memref descriptor and define static size
    // as a constant.
    if (shape[index] == -1) {
      // Find the position of the dynamic dimension in the list of dynamic sizes
      // by counting the number of preceding dynamic dimensions.  Start from 1
      // because the buffer pointer is at position zero.
      int64_t position = 1;
      for (uint64_t i = 0; i < index; ++i) {
        if (shape[i] == -1)
          ++position;
      }
      rewriter.replaceOpWithNewOp<LLVM::ExtractValueOp>(
          op, getIndexType(), operands[0],
          getIntegerArrayAttr(rewriter, position));
    } else {
      rewriter.replaceOp(
          op, createIndexConstant(rewriter, op->getLoc(), shape[index]));
    }
  }
};

// Common base for load and store operations on MemRefs.  Restricts the match
// to supported MemRef types.  Provides functionality to emit code accessing a
// specific element of the underlying data buffer.
template <typename Derived>
struct LoadStoreOpLowering : public LLVMLegalizationPattern<Derived> {
  using LLVMLegalizationPattern<Derived>::LLVMLegalizationPattern;
  using Base = LoadStoreOpLowering<Derived>;

  PatternMatchResult match(Operation *op) const override {
    MemRefType type = cast<Derived>(op).getMemRefType();
    return isSupportedMemRefType(type) ? this->matchSuccess()
                                       : this->matchFailure();
  }

  // Given subscript indices and array sizes in row-major order,
  //   i_n, i_{n-1}, ..., i_1
  //   s_n, s_{n-1}, ..., s_1
  // obtain a value that corresponds to the linearized subscript
  //   \sum_k i_k * \prod_{j=1}^{k-1} s_j
  // by accumulating the running linearized value.
  // Note that `indices` and `allocSizes` are passed in the same order as they
  // appear in load/store operations and memref type declarations.
  Value *linearizeSubscripts(PatternRewriter &builder, Location loc,
                             ArrayRef<Value *> indices,
                             ArrayRef<Value *> allocSizes) const {
    assert(indices.size() == allocSizes.size() &&
           "mismatching number of indices and allocation sizes");
    assert(!indices.empty() && "cannot linearize a 0-dimensional access");

    Value *linearized = indices.front();
    for (int i = 1, nSizes = allocSizes.size(); i < nSizes; ++i) {
      linearized = builder.create<LLVM::MulOp>(
          loc, this->getIndexType(),
          ArrayRef<Value *>{linearized, allocSizes[i]});
      linearized = builder.create<LLVM::AddOp>(
          loc, this->getIndexType(), ArrayRef<Value *>{linearized, indices[i]});
    }
    return linearized;
  }

  // Given the MemRef type, a descriptor and a list of indices, extract the data
  // buffer pointer from the descriptor, convert multi-dimensional subscripts
  // into a linearized index (using dynamic size data from the descriptor if
  // necessary) and get the pointer to the buffer element identified by the
  // indices.
  Value *getElementPtr(Location loc, Type elementTypePtr,
                       ArrayRef<int64_t> shape, Value *memRefDescriptor,
                       ArrayRef<Value *> indices,
                       PatternRewriter &rewriter) const {
    // Get the list of MemRef sizes.  Static sizes are defined as constants.
    // Dynamic sizes are extracted from the MemRef descriptor, where they start
    // from the position 1 (the buffer is at position 0).
    SmallVector<Value *, 4> sizes;
    unsigned dynamicSizeIdx = 1;
    for (int64_t s : shape) {
      if (s == -1) {
        Value *size = rewriter.create<LLVM::ExtractValueOp>(
            loc, this->getIndexType(), memRefDescriptor,
            this->getIntegerArrayAttr(rewriter, dynamicSizeIdx++));
        sizes.push_back(size);
      } else {
        sizes.push_back(this->createIndexConstant(rewriter, loc, s));
      }
    }

    // The second and subsequent operands are access subscripts.  Obtain the
    // linearized address in the buffer.
    Value *subscript = linearizeSubscripts(rewriter, loc, indices, sizes);

    Value *dataPtr = rewriter.create<LLVM::ExtractValueOp>(
        loc, elementTypePtr, memRefDescriptor,
        this->getIntegerArrayAttr(rewriter, 0));
    return rewriter.create<LLVM::GEPOp>(loc, elementTypePtr,
                                        ArrayRef<Value *>{dataPtr, subscript},
                                        ArrayRef<NamedAttribute>{});
  }
  // This is a getElementPtr variant, where the value is a direct raw pointer.
  // If a shape is empty, we are dealing with a zero-dimensional memref. Return
  // the pointer unmodified in this case.  Otherwise, linearize subscripts to
  // obtain the offset with respect to the base pointer.  Use this offset to
  // compute and return the element pointer.
  Value *getRawElementPtr(Location loc, Type elementTypePtr,
                          ArrayRef<int64_t> shape, Value *rawDataPtr,
                          ArrayRef<Value *> indices,
                          PatternRewriter &rewriter) const {
    if (shape.empty())
      return rawDataPtr;

    SmallVector<Value *, 4> sizes;
    for (int64_t s : shape) {
      sizes.push_back(this->createIndexConstant(rewriter, loc, s));
    }

    Value *subscript = linearizeSubscripts(rewriter, loc, indices, sizes);
    return rewriter.create<LLVM::GEPOp>(
        loc, elementTypePtr, ArrayRef<Value *>{rawDataPtr, subscript},
        ArrayRef<NamedAttribute>{});
  }

  Value *getDataPtr(Location loc, MemRefType type, Value *dataPtr,
                    ArrayRef<Value *> indices, PatternRewriter &rewriter,
                    llvm::Module &module) const {
    auto ptrType = getMemRefElementPtrType(type, this->lowering);
    auto shape = type.getShape();
    if (type.hasStaticShape()) {
      // NB: If memref was statically-shaped, dataPtr is pointer to raw data.
      return getRawElementPtr(loc, ptrType, shape, dataPtr, indices, rewriter);
    }
    return getElementPtr(loc, ptrType, shape, dataPtr, indices, rewriter);
  }
};

// Load operation is lowered to obtaining a pointer to the indexed element
// and loading it.
struct LoadOpLowering : public LoadStoreOpLowering<LoadOp> {
  using Base::Base;

  void rewrite(Operation *op, ArrayRef<Value *> operands,
               PatternRewriter &rewriter) const override {
    auto loadOp = cast<LoadOp>(op);
    auto type = loadOp.getMemRefType();

    Value *dataPtr = getDataPtr(op->getLoc(), type, operands.front(),
                                operands.drop_front(), rewriter, getModule());
    auto elementType = lowering.convertType(type.getElementType());

    rewriter.replaceOpWithNewOp<LLVM::LoadOp>(op, elementType,
                                              ArrayRef<Value *>{dataPtr});
  }
};

// Store opreation is lowered to obtaining a pointer to the indexed element,
// and storing the given value to it.
struct StoreOpLowering : public LoadStoreOpLowering<StoreOp> {
  using Base::Base;

  void rewrite(Operation *op, ArrayRef<Value *> operands,
               PatternRewriter &rewriter) const override {
    auto type = cast<StoreOp>(op).getMemRefType();

    Value *dataPtr = getDataPtr(op->getLoc(), type, operands[1],
                                operands.drop_front(2), rewriter, getModule());
    rewriter.create<LLVM::StoreOp>(op->getLoc(), operands[0], dataPtr);
  }
};

// Base class for LLVM IR lowering terminator operations with successors.
template <typename SourceOp, typename TargetOp>
struct OneToOneLLVMTerminatorLowering
    : public LLVMLegalizationPattern<SourceOp> {
  using LLVMLegalizationPattern<SourceOp>::LLVMLegalizationPattern;
  using Super = OneToOneLLVMTerminatorLowering<SourceOp, TargetOp>;

  void rewrite(Operation *op, ArrayRef<Value *> properOperands,
               ArrayRef<Block *> destinations,
               ArrayRef<ArrayRef<Value *>> operands,
               PatternRewriter &rewriter) const override {
    rewriter.create<TargetOp>(op->getLoc(), properOperands, destinations,
                              operands, op->getAttrs());
  }
};

// Special lowering pattern for `ReturnOps`.  Unlike all other operations,
// `ReturnOp` interacts with the function signature and must have as many
// operands as the function has return values.  Because in LLVM IR, functions
// can only return 0 or 1 value, we pack multiple values into a structure type.
// Emit `UndefOp` followed by `InsertValueOp`s to create such structure if
// necessary before returning it
struct ReturnOpLowering : public LLVMLegalizationPattern<ReturnOp> {
  using LLVMLegalizationPattern<ReturnOp>::LLVMLegalizationPattern;

  void rewrite(Operation *op, ArrayRef<Value *> operands,
               PatternRewriter &rewriter) const override {
    unsigned numArguments = op->getNumOperands();

    // If ReturnOp has 0 or 1 operand, create it and return immediately.
    if (numArguments == 0) {
      rewriter.create<LLVM::ReturnOp>(
          op->getLoc(), llvm::ArrayRef<Value *>(), llvm::ArrayRef<Block *>(),
          llvm::ArrayRef<llvm::ArrayRef<Value *>>(), op->getAttrs());
      return;
    }
    if (numArguments == 1) {
      rewriter.create<LLVM::ReturnOp>(
          op->getLoc(), llvm::ArrayRef<Value *>(operands.front()),
          llvm::ArrayRef<Block *>(), llvm::ArrayRef<llvm::ArrayRef<Value *>>(),
          op->getAttrs());
      return;
    }

    // Otherwise, we need to pack the arguments into an LLVM struct type before
    // returning.
    auto packedType = lowering.packFunctionResults(getTypes(op->getOperands()));

    Value *packed = rewriter.create<LLVM::UndefOp>(op->getLoc(), packedType);
    for (unsigned i = 0; i < numArguments; ++i) {
      packed = rewriter.create<LLVM::InsertValueOp>(
          op->getLoc(), packedType, packed, operands[i],
          getIntegerArrayAttr(rewriter, i));
    }
    rewriter.create<LLVM::ReturnOp>(
        op->getLoc(), llvm::makeArrayRef(packed), llvm::ArrayRef<Block *>(),
        llvm::ArrayRef<llvm::ArrayRef<Value *>>(), op->getAttrs());
  }
};

// FIXME: this should be tablegen'ed as well.
struct BranchOpLowering
    : public OneToOneLLVMTerminatorLowering<BranchOp, LLVM::BrOp> {
  using Super::Super;
};
struct CondBranchOpLowering
    : public OneToOneLLVMTerminatorLowering<CondBranchOp, LLVM::CondBrOp> {
  using Super::Super;
};

} // namespace

static void ensureDistinctSuccessors(Block &bb) {
  auto *terminator = bb.getTerminator();

  // Find repeated successors with arguments.
  llvm::SmallDenseMap<Block *, llvm::SmallVector<int, 4>> successorPositions;
  for (int i = 0, e = terminator->getNumSuccessors(); i < e; ++i) {
    Block *successor = terminator->getSuccessor(i);
    // Blocks with no arguments are safe even if they appear multiple times
    // because they don't need PHI nodes.
    if (successor->getNumArguments() == 0)
      continue;
    successorPositions[successor].push_back(i);
  }

  // If a successor appears for the second or more time in the terminator,
  // create a new dummy block that unconditionally branches to the original
  // destination, and retarget the terminator to branch to this new block.
  // There is no need to pass arguments to the dummy block because it will be
  // dominated by the original block and can therefore use any values defined in
  // the original block.
  for (const auto &successor : successorPositions) {
    const auto &positions = successor.second;
    // Start from the second occurrence of a block in the successor list.
    for (auto position = std::next(positions.begin()), end = positions.end();
         position != end; ++position) {
      auto *dummyBlock = new Block();
      bb.getParent()->push_back(dummyBlock);
      auto builder = FuncBuilder(dummyBlock);
      SmallVector<Value *, 8> operands(
          terminator->getSuccessorOperands(*position));
      builder.create<BranchOp>(terminator->getLoc(), successor.first, operands);
      terminator->setSuccessor(dummyBlock, *position);
      for (int i = 0, e = terminator->getNumSuccessorOperands(*position); i < e;
           ++i)
        terminator->eraseSuccessorOperand(*position, i);
    }
  }
}

void mlir::LLVM::ensureDistinctSuccessors(Module *m) {
  for (auto &f : *m) {
    for (auto &bb : f.getBlocks()) {
      ::ensureDistinctSuccessors(bb);
    }
  }
}

// Create a set of converters that live in the pass object by passing them a
// reference to the LLVM IR dialect.  Store the module associated with the
// dialect for further type conversion.
void LLVMLowering::initConverters(OwningRewritePatternList &patterns,
                                  MLIRContext *mlirContext) {
  llvmDialect = mlirContext->getRegisteredDialect<LLVM::LLVMDialect>();
  if (!llvmDialect) {
    mlirContext->emitError(UnknownLoc::get(mlirContext),
                           "LLVM IR dialect is not registered");
    return;
  }

  module = &llvmDialect->getLLVMModule();

  // FIXME: this should be tablegen'ed
  ConversionListBuilder<
      AddFOpLowering, AddIOpLowering, AndOpLowering, AllocOpLowering,
      BranchOpLowering, CallIndirectOpLowering, CallOpLowering, CmpIOpLowering,
      CondBranchOpLowering, ConstLLVMOpLowering, DeallocOpLowering,
      DimOpLowering, DivISOpLowering, DivIUOpLowering, DivFOpLowering,
      LoadOpLowering, MemRefCastOpLowering, MulFOpLowering, MulIOpLowering,
      OrOpLowering, RemISOpLowering, RemIUOpLowering, RemFOpLowering,
      ReturnOpLowering, SelectOpLowering, StoreOpLowering, SubFOpLowering,
      SubIOpLowering, XOrOpLowering>::build(patterns, *llvmDialect, *this);
  initAdditionalConverters(patterns);
}

// Convert types using the stored LLVM IR module.
Type LLVMLowering::convertType(Type t) {
  if (auto result = convertStandardType(t))
    return result;
  if (auto result = convertAdditionalType(t))
    return result;

  auto *mlirContext = llvmDialect->getContext();
  mlirContext->emitError(UnknownLoc::get(mlirContext))
      << "unsupported type: " << t;
  return {};
}

static llvm::Type *unwrapType(Type type) {
  return type.cast<LLVM::LLVMType>().getUnderlyingType();
}

// Create an LLVM IR structure type if there is more than one result.
Type LLVMLowering::packFunctionResults(ArrayRef<Type> types) {
  assert(!types.empty() && "expected non-empty list of type");

  if (types.size() == 1)
    return convertType(types.front());

  SmallVector<llvm::Type *, 8> resultTypes;
  resultTypes.reserve(types.size());
  for (auto t : types) {
    Type converted = convertType(t);
    if (!converted)
      return {};
    resultTypes.push_back(unwrapType(converted));
  }

  return LLVM::LLVMType::get(
      llvmDialect->getContext(),
      llvm::StructType::get(llvmDialect->getLLVMContext(), resultTypes));
}

// Convert function signatures using the stored LLVM IR module.
FunctionType LLVMLowering::convertFunctionSignatureType(
    FunctionType type, ArrayRef<NamedAttributeList> argAttrs,
    SmallVectorImpl<NamedAttributeList> &convertedArgAttrs) {

  convertedArgAttrs.reserve(argAttrs.size());
  for (auto attr : argAttrs)
    convertedArgAttrs.push_back(attr);

  SmallVector<Type, 8> argTypes;
  for (auto t : type.getInputs()) {
    auto converted = convertType(t);
    if (!converted)
      return {};
    argTypes.push_back(converted);
  }

  // If function does not return anything, return immediately.
  if (type.getNumResults() == 0)
    return FunctionType::get(argTypes, {}, type.getContext());

  // Otherwise pack the result types into a struct.
  if (auto result = packFunctionResults(type.getResults()))
    return FunctionType::get(argTypes, result, result.getContext());

  return {};
}

namespace {
// Make sure LLVM conversion pass errors out on the unsupported types instead
// of keeping them as is and resulting in a more cryptic verifier error.
class LLVMStandardLowering : public LLVMLowering {
protected:
  Type convertAdditionalType(Type) override { return {}; }
};
} // namespace

/// A pass converting MLIR Standard operations into the LLVM IR dialect.
class LLVMLoweringPass : public ModulePass<LLVMLoweringPass> {
public:
  // Run the dialect converter on the module.
  void runOnModule() override {
    Module *m = &getModule();
    LLVM::ensureDistinctSuccessors(m);
    if (failed(impl.convert(m)))
      signalPassFailure();
  }

private:
  LLVMStandardLowering impl;
};

ModulePassBase *mlir::createConvertToLLVMIRPass() {
  return new LLVMLoweringPass();
}

std::unique_ptr<DialectConversion> mlir::createStdToLLVMConverter() {
  return llvm::make_unique<LLVMLowering>();
}

static PassRegistration<LLVMLoweringPass>
    pass("lower-to-llvm", "Convert all functions to the LLVM IR dialect");
