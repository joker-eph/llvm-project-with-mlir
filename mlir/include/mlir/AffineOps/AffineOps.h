//===- AffineOps.h - MLIR Affine Operations -------------------------------===//
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
// This file defines convenience types for working with Affine operations
// in the MLIR operation set.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_AFFINEOPS_AFFINEOPS_H
#define MLIR_AFFINEOPS_AFFINEOPS_H

#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/StandardTypes.h"

namespace mlir {
class AffineBound;
class AffineValueMap;
class FlatAffineConstraints;
class FuncBuilder;

/// A utility function to check if a value is defined at the top level of a
/// function. A value defined at the top level is always a valid symbol.
bool isTopLevelSymbol(Value *value);

class AffineOpsDialect : public Dialect {
public:
  AffineOpsDialect(MLIRContext *context);
  static StringRef getDialectNamespace() { return "affine"; }
};

/// The "affine.apply" operation applies an affine map to a list of operands,
/// yielding a single result. The operand list must be the same size as the
/// number of arguments to the affine mapping.  All operands and the result are
/// of type 'Index'. This operation requires a single affine map attribute named
/// "map".  For example:
///
///   %y = "affine.apply" (%x) { map: (d0) -> (d0 + 1) } :
///          (index) -> (index)
///
/// equivalently:
///
///   #map42 = (d0)->(d0+1)
///   %y = affine.apply #map42(%x)
///
class AffineApplyOp : public Op<AffineApplyOp, OpTrait::VariadicOperands,
                                OpTrait::OneResult, OpTrait::HasNoSideEffect> {
public:
  using Op::Op;

  /// Builds an affine apply op with the specified map and operands.
  static void build(Builder *builder, OperationState *result, AffineMap map,
                    ArrayRef<Value *> operands);

  /// Returns the affine map to be applied by this operation.
  AffineMap getAffineMap() {
    return getAttrOfType<AffineMapAttr>("map").getValue();
  }

  /// Returns true if the result of this operation can be used as dimension id.
  bool isValidDim();

  /// Returns true if the result of this operation is a symbol.
  bool isValidSymbol();

  static StringRef getOperationName() { return "affine.apply"; }

  // Hooks to customize behavior of this op.
  static ParseResult parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  LogicalResult verify();
  OpFoldResult fold(ArrayRef<Attribute> operands);

  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);
};

/// The "affine.for" operation represents an affine loop nest, defining an SSA
/// value for its induction variable. It has one region capturing the loop body.
/// The induction variable is represented as a argument of this region. This SSA
/// value always has type index, which is the size of the machine word. The
/// stride, represented by step, is a positive constant integer which defaults
/// to "1" if not present. The lower and upper bounds specify a half-open range:
/// the range includes the lower bound but does not include the upper bound.
///
/// The body region must contain exactly one block that terminates with
/// "affine.terminator".  Calling AffineForOp::build will create such region
/// and insert the terminator, so will the parsing even in cases if it is absent
/// from the custom format.
///
/// The lower and upper bounds of a for operation are represented as an
/// application of an affine mapping to a list of SSA values passed to the map.
/// The same restrictions hold for these SSA values as for all bindings of SSA
/// values to dimensions and symbols. The affine mappings for the bounds may
/// return multiple results, in which case the max/min keywords are required
/// (for the lower/upper bound respectively), and the bound is the
/// maximum/minimum of the returned values.
///
/// Example:
///
///   affine.for %i = 1 to 10 {
///     ...
///   }
///
class AffineForOp
    : public Op<AffineForOp, OpTrait::VariadicOperands, OpTrait::ZeroResult> {
public:
  using Op::Op;

  // Hooks to customize behavior of this op.
  static void build(Builder *builder, OperationState *result,
                    ArrayRef<Value *> lbOperands, AffineMap lbMap,
                    ArrayRef<Value *> ubOperands, AffineMap ubMap,
                    int64_t step = 1);
  static void build(Builder *builder, OperationState *result, int64_t lb,
                    int64_t ub, int64_t step = 1);
  LogicalResult verify();
  static ParseResult parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);

  static void getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context);

  static StringRef getOperationName() { return "affine.for"; }
  static StringRef getStepAttrName() { return "step"; }
  static StringRef getLowerBoundAttrName() { return "lower_bound"; }
  static StringRef getUpperBoundAttrName() { return "upper_bound"; }

  /// Return a Builder set up to insert operations immediately before the
  /// terminator.
  FuncBuilder getBodyBuilder();

  /// Get the body of the AffineForOp.
  Block *getBody() { return &getRegion().front(); }

  /// Get the body region of the AffineForOp.
  Region &getRegion() { return getOperation()->getRegion(0); }

  /// Returns the induction variable for this loop.
  Value *getInductionVar();

  //===--------------------------------------------------------------------===//
  // Bounds and step
  //===--------------------------------------------------------------------===//

  // TODO: provide iterators for the lower and upper bound operands
  // if the current access via getLowerBound(), getUpperBound() is too slow.

  /// Returns operands for the lower bound map.
  operand_range getLowerBoundOperands();

  /// Returns operands for the upper bound map.
  operand_range getUpperBoundOperands();

  /// Returns information about the lower bound as a single object.
  AffineBound getLowerBound();

  /// Returns information about the upper bound as a single object.
  AffineBound getUpperBound();

  /// Returns loop step.
  int64_t getStep() {
    return getAttr(getStepAttrName()).cast<IntegerAttr>().getInt();
  }

  /// Returns affine map for the lower bound.
  AffineMap getLowerBoundMap() { return getLowerBoundMapAttr().getValue(); }
  AffineMapAttr getLowerBoundMapAttr() {
    return getAttr(getLowerBoundAttrName()).cast<AffineMapAttr>();
  }
  /// Returns affine map for the upper bound. The upper bound is exclusive.
  AffineMap getUpperBoundMap() { return getUpperBoundMapAttr().getValue(); }
  AffineMapAttr getUpperBoundMapAttr() {
    return getAttr(getUpperBoundAttrName()).cast<AffineMapAttr>();
  }

  /// Set lower bound. The new bound must have the same number of operands as
  /// the current bound map. Otherwise, 'replaceForLowerBound' should be used.
  void setLowerBound(ArrayRef<Value *> operands, AffineMap map);
  /// Set upper bound. The new bound must not have more operands than the
  /// current bound map. Otherwise, 'replaceForUpperBound' should be used.
  void setUpperBound(ArrayRef<Value *> operands, AffineMap map);

  /// Set the lower bound map without changing operands.
  void setLowerBoundMap(AffineMap map);

  /// Set the upper bound map without changing operands.
  void setUpperBoundMap(AffineMap map);

  /// Set loop step.
  void setStep(int64_t step) {
    assert(step > 0 && "step has to be a positive integer constant");
    auto *context = getLowerBoundMap().getContext();
    setAttr(Identifier::get(getStepAttrName(), context),
            IntegerAttr::get(IndexType::get(context), step));
  }

  /// Returns true if the lower bound is constant.
  bool hasConstantLowerBound();
  /// Returns true if the upper bound is constant.
  bool hasConstantUpperBound();
  /// Returns true if both bounds are constant.
  bool hasConstantBounds() {
    return hasConstantLowerBound() && hasConstantUpperBound();
  }
  /// Returns the value of the constant lower bound.
  /// Fails assertion if the bound is non-constant.
  int64_t getConstantLowerBound();
  /// Returns the value of the constant upper bound. The upper bound is
  /// exclusive. Fails assertion if the bound is non-constant.
  int64_t getConstantUpperBound();
  /// Sets the lower bound to the given constant value.
  void setConstantLowerBound(int64_t value);
  /// Sets the upper bound to the given constant value.
  void setConstantUpperBound(int64_t value);

  /// Returns true if both the lower and upper bound have the same operand lists
  /// (same operands in the same order).
  bool matchingBoundOperandList();
};

/// Returns if the provided value is the induction variable of a AffineForOp.
bool isForInductionVar(Value *val);

/// Returns the loop parent of an induction variable. If the provided value is
/// not an induction variable, then return nullptr.
AffineForOp getForInductionVarOwner(Value *val);

/// Extracts the induction variables from a list of AffineForOps and places them
/// in the output argument `ivs`.
void extractForInductionVars(ArrayRef<AffineForOp> forInsts,
                             SmallVectorImpl<Value *> *ivs);

/// AffineBound represents a lower or upper bound in the for operation.
/// This class does not own the underlying operands. Instead, it refers
/// to the operands stored in the AffineForOp. Its life span should not exceed
/// that of the for operation it refers to.
class AffineBound {
public:
  AffineForOp getAffineForOp() { return op; }
  AffineMap getMap() { return map; }

  /// Returns an AffineValueMap representing this bound.
  AffineValueMap getAsAffineValueMap();

  unsigned getNumOperands() { return opEnd - opStart; }
  Value *getOperand(unsigned idx) {
    return op.getOperation()->getOperand(opStart + idx);
  }

  using operand_iterator = AffineForOp::operand_iterator;
  using operand_range = AffineForOp::operand_range;

  operand_iterator operand_begin() { return op.operand_begin() + opStart; }
  operand_iterator operand_end() { return op.operand_begin() + opEnd; }
  operand_range getOperands() { return {operand_begin(), operand_end()}; }

private:
  // 'affine.for' operation that contains this bound.
  AffineForOp op;
  // Start and end positions of this affine bound operands in the list of
  // the containing 'affine.for' operation operands.
  unsigned opStart, opEnd;
  // Affine map for this bound.
  AffineMap map;

  AffineBound(AffineForOp op, unsigned opStart, unsigned opEnd, AffineMap map)
      : op(op), opStart(opStart), opEnd(opEnd), map(map) {}

  friend class AffineForOp;
};

/// The "if" operation represents an if-then-else construct for conditionally
/// executing two regions of code. The operands to an if operation are an
/// IntegerSet condition and a set of symbol/dimension operands to the
/// condition set. The operation produces no results. For example:
///
///    affine.if #set(%i)  {
///      ...
///    } else {
///      ...
///    }
///
/// The 'else' blocks to the if operation are optional, and may be omitted. For
/// example:
///
///    affine.if #set(%i)  {
///      ...
///    }
///
class AffineIfOp
    : public Op<AffineIfOp, OpTrait::VariadicOperands, OpTrait::ZeroResult> {
public:
  using Op::Op;

  // Hooks to customize behavior of this op.
  static void build(Builder *builder, OperationState *result,
                    IntegerSet condition, ArrayRef<Value *> conditionOperands);

  static StringRef getOperationName() { return "affine.if"; }
  static StringRef getConditionAttrName() { return "condition"; }

  IntegerSet getIntegerSet();
  void setIntegerSet(IntegerSet newSet);

  /// Returns the 'then' region.
  Region &getThenBlocks();

  /// Returns the 'else' blocks.
  Region &getElseBlocks();

  LogicalResult verify();
  static ParseResult parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
};

/// Affine terminator is a special terminator operation for blocks inside affine
/// loops and branches. It unconditionally transmits the control flow to the
/// successor of the operation enclosing the region.
///
/// This operation does _not_ have a custom syntax. However, affine control
/// operations omit the terminator in their custom syntax for brevity.
class AffineTerminatorOp
    : public Op<AffineTerminatorOp, OpTrait::ZeroOperands, OpTrait::ZeroResult,
                OpTrait::IsTerminator> {
public:
  using Op::Op;

  static void build(Builder *, OperationState *) {}

  static StringRef getOperationName() { return "affine.terminator"; }
};

/// Returns true if the given Value can be used as a dimension id.
bool isValidDim(Value *value);

/// Returns true if the given Value can be used as a symbol.
bool isValidSymbol(Value *value);

/// Modifies both `map` and `operands` in-place so as to:
/// 1. drop duplicate operands
/// 2. drop unused dims and symbols from map
void canonicalizeMapAndOperands(AffineMap *map,
                                llvm::SmallVectorImpl<Value *> *operands);

/// Returns a composed AffineApplyOp by composing `map` and `operands` with
/// other AffineApplyOps supplying those operands. The operands of the resulting
/// AffineApplyOp do not change the length of  AffineApplyOp chains.
AffineApplyOp makeComposedAffineApply(FuncBuilder *b, Location loc,
                                      AffineMap map,
                                      llvm::ArrayRef<Value *> operands);

/// Given an affine map `map` and its input `operands`, this method composes
/// into `map`, maps of AffineApplyOps whose results are the values in
/// `operands`, iteratively until no more of `operands` are the result of an
/// AffineApplyOp. When this function returns, `map` becomes the composed affine
/// map, and each Value in `operands` is guaranteed to be either a loop IV or a
/// terminal symbol, i.e., a symbol defined at the top level or a block/function
/// argument.
void fullyComposeAffineMapAndOperands(AffineMap *map,
                                      llvm::SmallVectorImpl<Value *> *operands);

} // end namespace mlir

#endif
