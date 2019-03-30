//===- Ops.h - Standard MLIR Operations -------------------------*- C++ -*-===//
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
// This file defines convenience types for working with standard operations
// in the MLIR instruction set.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_STANDARDOPS_OPS_H
#define MLIR_STANDARDOPS_OPS_H

#include "mlir/IR/Attributes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/StandardTypes.h"

namespace mlir {
class AffineMap;
class Builder;

namespace detail {
/// A custom binary operation printer that omits the "std." prefix from the
/// operation names.
void printStandardBinaryOp(Instruction *op, OpAsmPrinter *p);
} // namespace detail

class StandardOpsDialect : public Dialect {
public:
  StandardOpsDialect(MLIRContext *context);
};

#define GET_OP_CLASSES
#include "mlir/StandardOps/Ops.h.inc"

/// The "alloc" operation allocates a region of memory, as specified by its
/// memref type. For example:
///
///   %0 = alloc() : memref<8x64xf32, (d0, d1) -> (d0, d1), 1>
///
/// The optional list of dimension operands are bound to the dynamic dimensions
/// specified in its memref type. In the example below, the ssa value '%d' is
/// bound to the second dimension of the memref (which is dynamic).
///
///   %0 = alloc(%d) : memref<8x?xf32, (d0, d1) -> (d0, d1), 1>
///
/// The optional list of symbol operands are bound to the symbols of the
/// memrefs affine map. In the example below, the ssa value '%s' is bound to
/// the symbol 's0' in the affine map specified in the allocs memref type.
///
///   %0 = alloc()[%s] : memref<8x64xf32, (d0, d1)[s0] -> ((d0 + s0), d1), 1>
///
/// This operation returns a single ssa value of memref type, which can be used
/// by subsequent load and store operations.
class AllocOp
    : public Op<AllocOp, OpTrait::VariadicOperands, OpTrait::OneResult> {
public:
  using Op::Op;

  /// The result of an alloc is always a MemRefType.
  MemRefType getType() { return getResult()->getType().cast<MemRefType>(); }

  static StringRef getOperationName() { return "std.alloc"; }

  // Hooks to customize behavior of this op.
  static void build(Builder *builder, OperationState *result,
                    MemRefType memrefType, ArrayRef<Value *> operands = {});
  bool verify();
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);
};

/// The "br" operation represents a branch instruction in a function.
/// The operation takes variable number of operands and produces no results.
/// The operand number and types for each successor must match the
/// arguments of the block successor. For example:
///
///   ^bb2:
///      %2 = call @someFn()
///      br ^bb3(%2 : tensor<*xf32>)
///   ^bb3(%3: tensor<*xf32>):
///
class BranchOp : public Op<BranchOp, OpTrait::VariadicOperands,
                           OpTrait::ZeroResult, OpTrait::IsTerminator> {
public:
  using Op::Op;

  static StringRef getOperationName() { return "std.br"; }

  static void build(Builder *builder, OperationState *result, Block *dest,
                    ArrayRef<Value *> operands = {});

  // Hooks to customize behavior of this op.
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);

  /// Return the block this branch jumps to.
  Block *getDest();
  void setDest(Block *block);

  /// Erase the operand at 'index' from the operand list.
  void eraseOperand(unsigned index);
};

/// The "call" operation represents a direct call to a function.  The operands
/// and result types of the call must match the specified function type.  The
/// callee is encoded as a function attribute named "callee".
///
///   %31 = call @my_add(%0, %1)
///            : (tensor<16xf32>, tensor<16xf32>) -> tensor<16xf32>
class CallOp
    : public Op<CallOp, OpTrait::VariadicOperands, OpTrait::VariadicResults> {
public:
  using Op::Op;

  static StringRef getOperationName() { return "std.call"; }

  static void build(Builder *builder, OperationState *result, Function *callee,
                    ArrayRef<Value *> operands);

  Function *getCallee() {
    return getAttrOfType<FunctionAttr>("callee").getValue();
  }

  /// Get the argument operands to the called function.
  llvm::iterator_range<operand_iterator> getArgOperands() {
    return {arg_operand_begin(), arg_operand_end()};
  }

  operand_iterator arg_operand_begin() { return operand_begin(); }
  operand_iterator arg_operand_end() { return operand_end(); }

  // Hooks to customize behavior of this op.
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  bool verify();
};

/// The "call_indirect" operation represents an indirect call to a value of
/// function type.  Functions are first class types in MLIR, and may be passed
/// as arguments and merged together with block arguments.  The operands
/// and result types of the call must match the specified function type.
///
///   %31 = call_indirect %15(%0, %1)
///            : (tensor<16xf32>, tensor<16xf32>) -> tensor<16xf32>
///
class CallIndirectOp : public Op<CallIndirectOp, OpTrait::VariadicOperands,
                                 OpTrait::VariadicResults> {
public:
  using Op::Op;
  static StringRef getOperationName() { return "std.call_indirect"; }

  static void build(Builder *builder, OperationState *result, Value *callee,
                    ArrayRef<Value *> operands);

  Value *getCallee() { return getOperand(0); }

  /// Get the argument operands to the called function.
  llvm::iterator_range<operand_iterator> getArgOperands() {
    return {arg_operand_begin(), arg_operand_end()};
  }

  operand_iterator arg_operand_begin() { return ++operand_begin(); }
  operand_iterator arg_operand_end() { return operand_end(); }

  // Hooks to customize behavior of this op.
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  bool verify();
  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);
};

/// The predicate indicates the type of the comparison to perform:
/// (in)equality; (un)signed less/greater than (or equal to).
enum class CmpIPredicate {
  FirstValidValue,
  // (In)equality comparisons.
  EQ = FirstValidValue,
  NE,
  // Signed comparisons.
  SLT,
  SLE,
  SGT,
  SGE,
  // Unsigned comparisons.
  ULT,
  ULE,
  UGT,
  UGE,
  // Number of predicates.
  NumPredicates
};

/// The "cmpi" operation compares its two operands according to the integer
/// comparison rules and the predicate specified by the respective attribute.
/// The predicate defines the type of comparison: (in)equality, (un)signed
/// less/greater than (or equal to).  The operands must have the same type, and
/// this type must be an integer type, a vector or a tensor thereof.  The result
/// is an i1, or a vector/tensor thereof having the same shape as the inputs.
/// Since integers are signless, the predicate also explicitly indicates
/// whether to interpret the operands as signed or unsigned integers for
/// less/greater than comparisons.  For the sake of readability by humans,
/// custom assembly form for the instruction uses a string-typed attribute for
/// the predicate.  The value of this attribute corresponds to lower-cased name
/// of the predicate constant, e.g., "slt" means "signed less than".  The string
/// representation of the attribute is merely a syntactic sugar and is converted
/// to an integer attribute by the parser.
///
///   %r1 = cmpi "eq" %0, %1 : i32
///   %r2 = cmpi "slt" %0, %1 : tensor<42x42xi64>
///   %r3 = "std.cmpi"(%0, %1){predicate: 0} : (i8, i8) -> i1
class CmpIOp
    : public Op<CmpIOp, OpTrait::OperandsAreIntegerLike,
                OpTrait::SameTypeOperands, OpTrait::NOperands<2>::Impl,
                OpTrait::OneResult, OpTrait::ResultsAreBoolLike,
                OpTrait::SameOperandsAndResultShape, OpTrait::HasNoSideEffect> {
public:
  using Op::Op;

  CmpIPredicate getPredicate() {
    return (CmpIPredicate)getAttrOfType<IntegerAttr>(getPredicateAttrName())
        .getInt();
  }

  static StringRef getOperationName() { return "std.cmpi"; }
  static StringRef getPredicateAttrName() { return "predicate"; }
  static CmpIPredicate getPredicateByName(StringRef name);

  static void build(Builder *builder, OperationState *result, CmpIPredicate,
                    Value *lhs, Value *rhs);
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  bool verify();
  Attribute constantFold(ArrayRef<Attribute> operands, MLIRContext *context);
};

/// The "cond_br" operation represents a conditional branch instruction in a
/// function. The operation takes variable number of operands and produces
/// no results. The operand number and types for each successor must match the
//  arguments of the block successor. For example:
///
///   ^bb0:
///      %0 = extract_element %arg0[] : tensor<i1>
///      cond_br %0, ^bb1, ^bb2
///   ^bb1:
///      ...
///   ^bb2:
///      ...
///
class CondBranchOp : public Op<CondBranchOp, OpTrait::AtLeastNOperands<1>::Impl,
                               OpTrait::ZeroResult, OpTrait::IsTerminator> {
  // These are the indices into the dests list.
  enum { trueIndex = 0, falseIndex = 1 };

  /// The operands list of a conditional branch operation is layed out as
  /// follows:
  /// { condition, [true_operands], [false_operands] }
public:
  using Op::Op;

  static StringRef getOperationName() { return "std.cond_br"; }

  static void build(Builder *builder, OperationState *result, Value *condition,
                    Block *trueDest, ArrayRef<Value *> trueOperands,
                    Block *falseDest, ArrayRef<Value *> falseOperands);

  // Hooks to customize behavior of this op.
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  bool verify();

  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);

  // The condition operand is the first operand in the list.
  Value *getCondition() { return getOperand(0); }

  /// Return the destination if the condition is true.
  Block *getTrueDest();

  /// Return the destination if the condition is false.
  Block *getFalseDest();

  // Accessors for operands to the 'true' destination.
  Value *getTrueOperand(unsigned idx) {
    assert(idx < getNumTrueOperands());
    return getOperand(getTrueDestOperandIndex() + idx);
  }

  void setTrueOperand(unsigned idx, Value *value) {
    assert(idx < getNumTrueOperands());
    setOperand(getTrueDestOperandIndex() + idx, value);
  }

  operand_iterator true_operand_begin() {
    return operand_begin() + getTrueDestOperandIndex();
  }
  operand_iterator true_operand_end() {
    return true_operand_begin() + getNumTrueOperands();
  }
  llvm::iterator_range<operand_iterator> getTrueOperands() {
    return {true_operand_begin(), true_operand_end()};
  }

  unsigned getNumTrueOperands();

  /// Erase the operand at 'index' from the true operand list.
  void eraseTrueOperand(unsigned index);

  // Accessors for operands to the 'false' destination.
  Value *getFalseOperand(unsigned idx) {
    assert(idx < getNumFalseOperands());
    return getOperand(getFalseDestOperandIndex() + idx);
  }
  void setFalseOperand(unsigned idx, Value *value) {
    assert(idx < getNumFalseOperands());
    setOperand(getFalseDestOperandIndex() + idx, value);
  }

  operand_iterator false_operand_begin() { return true_operand_end(); }
  operand_iterator false_operand_end() {
    return false_operand_begin() + getNumFalseOperands();
  }
  llvm::iterator_range<operand_iterator> getFalseOperands() {
    return {false_operand_begin(), false_operand_end()};
  }

  unsigned getNumFalseOperands();

  /// Erase the operand at 'index' from the false operand list.
  void eraseFalseOperand(unsigned index);

private:
  /// Get the index of the first true destination operand.
  unsigned getTrueDestOperandIndex() { return 1; }

  /// Get the index of the first false destination operand.
  unsigned getFalseDestOperandIndex() {
    return getTrueDestOperandIndex() + getNumTrueOperands();
  }
};

/// The "constant" operation requires a single attribute named "value".
/// It returns its value as an SSA value.  For example:
///
///   %1 = "std.constant"(){value: 42} : i32
///   %2 = "std.constant"(){value: @foo} : (f32)->f32
///
class ConstantOp : public Op<ConstantOp, OpTrait::ZeroOperands,
                             OpTrait::OneResult, OpTrait::HasNoSideEffect> {
public:
  using Op::Op;

  /// Builds a constant op with the specified attribute value and result type.
  static void build(Builder *builder, OperationState *result, Type type,
                    Attribute value);

  /// Builds a constant op with the specified attribute value and the
  /// attribute's type.
  static void build(Builder *builder, OperationState *result, Attribute value);

  Attribute getValue() { return getAttr("value"); }

  static StringRef getOperationName() { return "std.constant"; }

  // Hooks to customize behavior of this op.
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  bool verify();
  Attribute constantFold(ArrayRef<Attribute> operands, MLIRContext *context);
};

/// This is a refinement of the "constant" op for the case where it is
/// returning a float value of FloatType.
///
///   %1 = "std.constant"(){value: 42.0} : bf16
///
class ConstantFloatOp : public ConstantOp {
public:
  using ConstantOp::ConstantOp;

  /// Builds a constant float op producing a float of the specified type.
  static void build(Builder *builder, OperationState *result,
                    const APFloat &value, FloatType type);

  APFloat getValue() { return getAttrOfType<FloatAttr>("value").getValue(); }

  static bool isClassFor(Instruction *op);
};

/// This is a refinement of the "constant" op for the case where it is
/// returning an integer value of IntegerType.
///
///   %1 = "std.constant"(){value: 42} : i32
///
class ConstantIntOp : public ConstantOp {
public:
  using ConstantOp::ConstantOp;
  /// Build a constant int op producing an integer of the specified width.
  static void build(Builder *builder, OperationState *result, int64_t value,
                    unsigned width);

  /// Build a constant int op producing an integer with the specified type,
  /// which must be an integer type.
  static void build(Builder *builder, OperationState *result, int64_t value,
                    Type type);

  int64_t getValue() { return getAttrOfType<IntegerAttr>("value").getInt(); }

  static bool isClassFor(Instruction *op);
};

/// This is a refinement of the "constant" op for the case where it is
/// returning an integer value of Index type.
///
///   %1 = "std.constant"(){value: 99} : () -> index
///
class ConstantIndexOp : public ConstantOp {
public:
  using ConstantOp::ConstantOp;

  /// Build a constant int op producing an index.
  static void build(Builder *builder, OperationState *result, int64_t value);

  int64_t getValue() { return getAttrOfType<IntegerAttr>("value").getInt(); }

  static bool isClassFor(Instruction *op);
};

/// The "dealloc" operation frees the region of memory referenced by a memref
/// which was originally created by the "alloc" operation.
/// The "dealloc" operation should not be called on memrefs which alias an
//  alloc'd memref (i.e. memrefs returned by the "view" and "reshape"
/// operations).
///
///   %0 = alloc() : memref<8x64xf32, (d0, d1) -> (d0, d1), 1>
///
///   dealloc %0 : memref<8x64xf32, (d0, d1) -> (d0, d1), 1>
///
class DeallocOp
    : public Op<DeallocOp, OpTrait::OneOperand, OpTrait::ZeroResult> {
public:
  using Op::Op;

  Value *getMemRef() { return getOperand(); }
  void setMemRef(Value *value) { setOperand(value); }

  static StringRef getOperationName() { return "std.dealloc"; }

  // Hooks to customize behavior of this op.
  static void build(Builder *builder, OperationState *result, Value *memref);
  bool verify();
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);
};

/// The "dim" operation takes a memref or tensor operand and returns an
/// "index".  It requires a single integer attribute named "index".  It
/// returns the size of the specified dimension.  For example:
///
///   %1 = dim %0, 2 : tensor<?x?x?xf32>
///
class DimOp : public Op<DimOp, OpTrait::OneOperand, OpTrait::OneResult,
                        OpTrait::HasNoSideEffect> {
public:
  using Op::Op;

  static void build(Builder *builder, OperationState *result,
                    Value *memrefOrTensor, unsigned index);

  Attribute constantFold(ArrayRef<Attribute> operands, MLIRContext *context);

  /// This returns the dimension number that the 'dim' is inspecting.
  unsigned getIndex() {
    return getAttrOfType<IntegerAttr>("index").getValue().getZExtValue();
  }

  static StringRef getOperationName() { return "std.dim"; }

  // Hooks to customize behavior of this op.
  bool verify();
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
};

// DmaStartOp starts a non-blocking DMA operation that transfers data from a
// source memref to a destination memref. The source and destination memref need
// not be of the same dimensionality, but need to have the same elemental type.
// The operands include the source and destination memref's each followed by its
// indices, size of the data transfer in terms of the number of elements (of the
// elemental type of the memref), a tag memref with its indices, and optionally
// at the end, a stride and a number_of_elements_per_stride arguments. The tag
// location is used by a DmaWaitOp to check for completion. The indices of the
// source memref, destination memref, and the tag memref have the same
// restrictions as any load/store. The optional stride arguments should be of
// 'index' type, and specify a stride for the slower memory space (memory space
// with a lower memory space id), tranferring chunks of
// number_of_elements_per_stride every stride until %num_elements are
// transferred. Either both or no stride arguments should be specified.
//
// For example, a DmaStartOp operation that transfers 256 elements of a memref
// '%src' in memory space 0 at indices [%i, %j] to memref '%dst' in memory space
// 1 at indices [%k, %l], would be specified as follows:
//
//   %num_elements = constant 256
//   %idx = constant 0 : index
//   %tag = alloc() : memref<1 x i32, (d0) -> (d0), 4>
//   dma_start %src[%i, %j], %dst[%k, %l], %num_elements, %tag[%idx] :
//     memref<40 x 128 x f32>, (d0) -> (d0), 0>,
//     memref<2 x 1024 x f32>, (d0) -> (d0), 1>,
//     memref<1 x i32>, (d0) -> (d0), 2>
//
//   If %stride and %num_elt_per_stride are specified, the DMA is expected to
//   transfer %num_elt_per_stride elements every %stride elements apart from
//   memory space 0 until %num_elements are transferred.
//
//   dma_start %src[%i, %j], %dst[%k, %l], %num_elements, %tag[%idx], %stride,
//             %num_elt_per_stride :
//
// TODO(mlir-team): add additional operands to allow source and destination
// striding, and multiple stride levels.
// TODO(andydavis) Consider replacing src/dst memref indices with view memrefs.
class DmaStartOp
    : public Op<DmaStartOp, OpTrait::VariadicOperands, OpTrait::ZeroResult> {
public:
  using Op::Op;

  static void build(Builder *builder, OperationState *result, Value *srcMemRef,
                    ArrayRef<Value *> srcIndices, Value *destMemRef,
                    ArrayRef<Value *> destIndices, Value *numElements,
                    Value *tagMemRef, ArrayRef<Value *> tagIndices,
                    Value *stride = nullptr,
                    Value *elementsPerStride = nullptr);

  // Returns the source MemRefType for this DMA operation.
  Value *getSrcMemRef() { return getOperand(0); }
  // Returns the rank (number of indices) of the source MemRefType.
  unsigned getSrcMemRefRank() {
    return getSrcMemRef()->getType().cast<MemRefType>().getRank();
  }
  // Returns the source memerf indices for this DMA operation.
  llvm::iterator_range<Instruction::operand_iterator> getSrcIndices() {
    return {getOperation()->operand_begin() + 1,
            getOperation()->operand_begin() + 1 + getSrcMemRefRank()};
  }

  // Returns the destination MemRefType for this DMA operations.
  Value *getDstMemRef() { return getOperand(1 + getSrcMemRefRank()); }
  // Returns the rank (number of indices) of the destination MemRefType.
  unsigned getDstMemRefRank() {
    return getDstMemRef()->getType().cast<MemRefType>().getRank();
  }
  unsigned getSrcMemorySpace() {
    return getSrcMemRef()->getType().cast<MemRefType>().getMemorySpace();
  }
  unsigned getDstMemorySpace() {
    return getDstMemRef()->getType().cast<MemRefType>().getMemorySpace();
  }

  // Returns the destination memref indices for this DMA operation.
  llvm::iterator_range<Instruction::operand_iterator> getDstIndices() {
    return {getOperation()->operand_begin() + 1 + getSrcMemRefRank() + 1,
            getOperation()->operand_begin() + 1 + getSrcMemRefRank() + 1 +
                getDstMemRefRank()};
  }

  // Returns the number of elements being transferred by this DMA operation.
  Value *getNumElements() {
    return getOperand(1 + getSrcMemRefRank() + 1 + getDstMemRefRank());
  }

  // Returns the Tag MemRef for this DMA operation.
  Value *getTagMemRef() {
    return getOperand(1 + getSrcMemRefRank() + 1 + getDstMemRefRank() + 1);
  }
  // Returns the rank (number of indices) of the tag MemRefType.
  unsigned getTagMemRefRank() {
    return getTagMemRef()->getType().cast<MemRefType>().getRank();
  }

  // Returns the tag memref index for this DMA operation.
  llvm::iterator_range<Instruction::operand_iterator> getTagIndices() {
    unsigned tagIndexStartPos =
        1 + getSrcMemRefRank() + 1 + getDstMemRefRank() + 1 + 1;
    return {getOperation()->operand_begin() + tagIndexStartPos,
            getOperation()->operand_begin() + tagIndexStartPos +
                getTagMemRefRank()};
  }

  /// Returns true if this is a DMA from a faster memory space to a slower one.
  bool isDestMemorySpaceFaster() {
    return (getSrcMemorySpace() < getDstMemorySpace());
  }

  /// Returns true if this is a DMA from a slower memory space to a faster one.
  bool isSrcMemorySpaceFaster() {
    // Assumes that a lower number is for a slower memory space.
    return (getDstMemorySpace() < getSrcMemorySpace());
  }

  /// Given a DMA start operation, returns the operand position of either the
  /// source or destination memref depending on the one that is at the higher
  /// level of the memory hierarchy. Asserts failure if neither is true.
  unsigned getFasterMemPos() {
    assert(isSrcMemorySpaceFaster() || isDestMemorySpaceFaster());
    return isSrcMemorySpaceFaster() ? 0 : getSrcMemRefRank() + 1;
  }

  static StringRef getOperationName() { return "std.dma_start"; }
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  bool verify();

  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);

  bool isStrided() {
    return getNumOperands() != 1 + getSrcMemRefRank() + 1 + getDstMemRefRank() +
                                   1 + 1 + getTagMemRefRank();
  }

  Value *getStride() {
    if (!isStrided())
      return nullptr;
    return getOperand(getNumOperands() - 1 - 1);
  }

  Value *getNumElementsPerStride() {
    if (!isStrided())
      return nullptr;
    return getOperand(getNumOperands() - 1);
  }
};

// DmaWaitOp blocks until the completion of a DMA operation associated with the
// tag element '%tag[%index]'. %tag is a memref, and %index has to be an index
// with the same restrictions as any load/store index. %num_elements is the
// number of elements associated with the DMA operation. For example:
//
//   dma_start %src[%i, %j], %dst[%k, %l], %num_elements, %tag[%index] :
//     memref<2048 x f32>, (d0) -> (d0), 0>,
//     memref<256 x f32>, (d0) -> (d0), 1>
//     memref<1 x i32>, (d0) -> (d0), 2>
//   ...
//   ...
//   dma_wait %tag[%index], %num_elements : memref<1 x i32, (d0) -> (d0), 2>
//
class DmaWaitOp
    : public Op<DmaWaitOp, OpTrait::VariadicOperands, OpTrait::ZeroResult> {
public:
  using Op::Op;

  static void build(Builder *builder, OperationState *result, Value *tagMemRef,
                    ArrayRef<Value *> tagIndices, Value *numElements);

  static StringRef getOperationName() { return "std.dma_wait"; }

  // Returns the Tag MemRef associated with the DMA operation being waited on.
  Value *getTagMemRef() { return getOperand(0); }

  // Returns the tag memref index for this DMA operation.
  llvm::iterator_range<Instruction::operand_iterator> getTagIndices() {
    return {getOperation()->operand_begin() + 1,
            getOperation()->operand_begin() + 1 + getTagMemRefRank()};
  }

  // Returns the rank (number of indices) of the tag memref.
  unsigned getTagMemRefRank() {
    return getTagMemRef()->getType().cast<MemRefType>().getRank();
  }

  // Returns the number of elements transferred in the associated DMA operation.
  Value *getNumElements() { return getOperand(1 + getTagMemRefRank()); }

  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);
};

/// The "extract_element" op reads a tensor or vector and returns one element
/// from it specified by an index list. The output of extract is a new value
/// with the same type as the elements of the tensor or vector. The arity of
/// indices matches the rank of the accessed value (i.e., if a tensor is of rank
/// 3, then 3 indices are required for the extract).  The indices should all be
/// of affine_int type.
///
/// For example:
///
///   %3 = extract_element %0[%1, %2] : vector<4x4xi32>
///
class ExtractElementOp
    : public Op<ExtractElementOp, OpTrait::VariadicOperands, OpTrait::OneResult,
                OpTrait::HasNoSideEffect> {
public:
  using Op::Op;

  static void build(Builder *builder, OperationState *result, Value *aggregate,
                    ArrayRef<Value *> indices = {});

  Value *getAggregate() { return getOperand(0); }

  llvm::iterator_range<Instruction::operand_iterator> getIndices() {
    return {getOperation()->operand_begin() + 1, getOperation()->operand_end()};
  }

  static StringRef getOperationName() { return "std.extract_element"; }

  // Hooks to customize behavior of this op.
  bool verify();
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  Attribute constantFold(ArrayRef<Attribute> operands, MLIRContext *context);
};

/// The "load" op reads an element from a memref specified by an index list. The
/// output of load is a new value with the same type as the elements of the
/// memref. The arity of indices is the rank of the memref (i.e., if the memref
/// loaded from is of rank 3, then 3 indices are required for the load following
/// the memref identifier).  For example:
///
///   %3 = load %0[%1, %1] : memref<4x4xi32>
///
class LoadOp
    : public Op<LoadOp, OpTrait::VariadicOperands, OpTrait::OneResult> {
public:
  using Op::Op;

  // Hooks to customize behavior of this op.
  static void build(Builder *builder, OperationState *result, Value *memref,
                    ArrayRef<Value *> indices = {});

  Value *getMemRef() { return getOperand(0); }
  void setMemRef(Value *value) { setOperand(0, value); }
  MemRefType getMemRefType() {
    return getMemRef()->getType().cast<MemRefType>();
  }

  llvm::iterator_range<Instruction::operand_iterator> getIndices() {
    return {getOperation()->operand_begin() + 1, getOperation()->operand_end()};
  }

  static StringRef getOperationName() { return "std.load"; }

  bool verify();
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);
};

/// The "memref_cast" operation converts a memref from one type to an equivalent
/// type with a compatible shape.  The source and destination types are
/// when both are memref types with the same element type, affine mappings,
/// address space, and rank but where the individual dimensions may add or
/// remove constant dimensions from the memref type.
///
/// If the cast converts any dimensions from an unknown to a known size, then it
/// acts as an assertion that fails at runtime of the dynamic dimensions
/// disagree with resultant destination size.
///
/// Assert that the input dynamic shape matches the destination static shape.
///    %2 = memref_cast %1 : memref<?x?xf32> to memref<4x4xf32>
/// Erase static shape information, replacing it with dynamic information.
///    %3 = memref_cast %1 : memref<4xf32> to memref<?xf32>
///
class MemRefCastOp : public CastOp<MemRefCastOp> {
public:
  using CastOp::CastOp;
  static StringRef getOperationName() { return "std.memref_cast"; }

  /// The result of a memref_cast is always a memref.
  MemRefType getType() { return getResult()->getType().cast<MemRefType>(); }

  void print(OpAsmPrinter *p);

  bool verify();
};

/// The "return" operation represents a return instruction within a function.
/// The operation takes variable number of operands and produces no results.
/// The operand number and types must match the signature of the function
/// that contains the operation. For example:
///
///   func @foo() : (i32, f8) {
///   ...
///   return %0, %1 : i32, f8
///
class ReturnOp : public Op<ReturnOp, OpTrait::VariadicOperands,
                           OpTrait::ZeroResult, OpTrait::IsTerminator> {
public:
  using Op::Op;

  static StringRef getOperationName() { return "std.return"; }

  static void build(Builder *builder, OperationState *result,
                    ArrayRef<Value *> results = {});

  // Hooks to customize behavior of this op.
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  bool verify();
};

/// The "select" operation chooses one value based on a binary condition
/// supplied as its first operand. If the value of the first operand is 1, the
/// second operand is chosen, otherwise the third operand is chosen. The second
/// and the third operand must have the same type. The operation applies
/// elementwise to vectors and tensors.  The shape of all arguments must be
/// identical. For example, the maximum operation is obtained by combining
/// "select" with "cmpi" as follows.
///
///   %2 = cmpi "gt" %0, %1 : i32         // %2 is i1
///   %3 = select %2, %0, %1 : i32
///
class SelectOp : public Op<SelectOp, OpTrait::NOperands<3>::Impl,
                           OpTrait::OneResult, OpTrait::HasNoSideEffect> {
public:
  using Op::Op;

  static StringRef getOperationName() { return "std.select"; }
  static void build(Builder *builder, OperationState *result, Value *condition,
                    Value *trueValue, Value *falseValue);
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  bool verify();

  Value *getCondition() { return getOperand(0); }
  Value *getTrueValue() { return getOperand(1); }
  Value *getFalseValue() { return getOperand(2); }

  Value *fold();
};

/// The "store" op writes an element to a memref specified by an index list.
/// The arity of indices is the rank of the memref (i.e. if the memref being
/// stored to is of rank 3, then 3 indices are required for the store following
/// the memref identifier). The store instruction does not produce a result.
///
/// In the following example, the ssa value '%v' is stored in memref '%A' at
/// indices [%i, %j]:
///
///   store %v, %A[%i, %j] : memref<4x128xf32, (d0, d1) -> (d0, d1), 0>
///
class StoreOp
    : public Op<StoreOp, OpTrait::VariadicOperands, OpTrait::ZeroResult> {
public:
  using Op::Op;

  // Hooks to customize behavior of this op.
  static void build(Builder *builder, OperationState *result,
                    Value *valueToStore, Value *memref,
                    ArrayRef<Value *> indices = {});

  Value *getValueToStore() { return getOperand(0); }

  Value *getMemRef() { return getOperand(1); }
  void setMemRef(Value *value) { setOperand(1, value); }
  MemRefType getMemRefType() {
    return getMemRef()->getType().cast<MemRefType>();
  }

  llvm::iterator_range<Instruction::operand_iterator> getIndices() {
    return {getOperation()->operand_begin() + 2, getOperation()->operand_end()};
  }

  static StringRef getOperationName() { return "std.store"; }

  bool verify();
  static bool parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);

  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);
};

/// The "tensor_cast" operation converts a tensor from one type to an equivalent
/// type without changing any data elements.  The source and destination types
/// must both be tensor types with the same element type, and the source and
/// destination types may not be the same.  They must either have the same rank,
/// or one may be an unknown rank.  The operation is invalid if converting to a
/// mismatching constant dimension.
///
/// Convert from unknown rank to rank 2 with unknown dimension sizes.
///    %2 = tensor_cast %1 : tensor<??f32> to tensor<?x?xf32>
///
class TensorCastOp : public CastOp<TensorCastOp> {
public:
  using CastOp::CastOp;

  static StringRef getOperationName() { return "std.tensor_cast"; }

  /// The result of a tensor_cast is always a tensor.
  TensorType getType() { return getResult()->getType().cast<TensorType>(); }

  void print(OpAsmPrinter *p);

  bool verify();
};

/// Prints dimension and symbol list.
void printDimAndSymbolList(Instruction::operand_iterator begin,
                           Instruction::operand_iterator end, unsigned numDims,
                           OpAsmPrinter *p);

/// Parses dimension and symbol list and returns true if parsing failed.
bool parseDimAndSymbolList(OpAsmParser *parser,
                           SmallVector<Value *, 4> &operands,
                           unsigned &numDims);

} // end namespace mlir

#endif // MLIR_STANDARDOPS_OPS_H
