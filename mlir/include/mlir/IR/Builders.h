//===- Builders.h - Helpers for constructing MLIR Classes -------*- C++ -*-===//
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

#ifndef MLIR_IR_BUILDERS_H
#define MLIR_IR_BUILDERS_H

#include "mlir/IR/CFGFunction.h"
#include "mlir/IR/MLFunction.h"
#include "mlir/IR/Statements.h"

namespace mlir {

class AffineExpr;
class Module;
class UnknownLoc;
class UniquedFilename;
class FileLineColLoc;
class Type;
class PrimitiveType;
class IntegerType;
class FunctionType;
class VectorType;
class RankedTensorType;
class UnrankedTensorType;
class BoolAttr;
class IntegerAttr;
class FloatAttr;
class StringAttr;
class TypeAttr;
class ArrayAttr;
class FunctionAttr;
class ElementsAttr;
class DenseElementsAttr;
class DenseIntElementsAttr;
class AffineMapAttr;
class AffineMap;

/// This class is a general helper class for creating context-global objects
/// like types, attributes, and affine expressions.
class Builder {
public:
  explicit Builder(MLIRContext *context) : context(context) {}
  explicit Builder(Module *module);

  MLIRContext *getContext() const { return context; }

  Identifier getIdentifier(StringRef str);
  Module *createModule();

  // Locations.
  UnknownLoc *getUnknownLoc();
  UniquedFilename getUniquedFilename(StringRef filename);
  FileLineColLoc *getFileLineColLoc(UniquedFilename filename, unsigned line,
                                    unsigned column);

  // Types.
  FloatType getBF16Type();
  FloatType getF16Type();
  FloatType getF32Type();
  FloatType getF64Type();

  OtherType getIndexType();
  OtherType getTFControlType();
  OtherType getTFStringType();
  OtherType getTFResourceType();
  OtherType getTFVariantType();
  OtherType getTFComplex64Type();
  OtherType getTFComplex128Type();
  OtherType getTFF32REFType();

  IntegerType getIntegerType(unsigned width);
  FunctionType getFunctionType(ArrayRef<Type> inputs, ArrayRef<Type> results);
  MemRefType getMemRefType(ArrayRef<int> shape, Type elementType,
                           ArrayRef<AffineMap> affineMapComposition = {},
                           unsigned memorySpace = 0);
  VectorType getVectorType(ArrayRef<int> shape, Type elementType);
  RankedTensorType getTensorType(ArrayRef<int> shape, Type elementType);
  UnrankedTensorType getTensorType(Type elementType);

  // Attributes.

  BoolAttr getBoolAttr(bool value);
  IntegerAttr getIntegerAttr(int64_t value);
  FloatAttr getFloatAttr(double value);
  FloatAttr getFloatAttr(const APFloat &value);
  StringAttr getStringAttr(StringRef bytes);
  ArrayAttr getArrayAttr(ArrayRef<Attribute> value);
  AffineMapAttr getAffineMapAttr(AffineMap map);
  IntegerSetAttr getIntegerSetAttr(IntegerSet set);
  TypeAttr getTypeAttr(Type type);
  FunctionAttr getFunctionAttr(const Function *value);
  ElementsAttr getSplatElementsAttr(VectorOrTensorType type, Attribute elt);
  ElementsAttr getDenseElementsAttr(VectorOrTensorType type,
                                    ArrayRef<char> data);
  ElementsAttr getSparseElementsAttr(VectorOrTensorType type,
                                     DenseIntElementsAttr indices,
                                     DenseElementsAttr values);
  ElementsAttr getOpaqueElementsAttr(VectorOrTensorType type, StringRef bytes);

  // Affine expressions and affine maps.
  AffineExpr getAffineDimExpr(unsigned position);
  AffineExpr getAffineSymbolExpr(unsigned position);
  AffineExpr getAffineConstantExpr(int64_t constant);

  AffineMap getAffineMap(unsigned dimCount, unsigned symbolCount,
                         ArrayRef<AffineExpr> results,
                         ArrayRef<AffineExpr> rangeSizes);

  // Special cases of affine maps and integer sets
  /// Returns a single constant result affine map with 0 dimensions and 0
  /// symbols.  One constant result: () -> (val).
  AffineMap getConstantAffineMap(int64_t val);
  // One dimension id identity map: (i) -> (i).
  AffineMap getDimIdentityMap();
  // Multi-dimensional identity map: (d0, d1, d2) -> (d0, d1, d2).
  AffineMap getMultiDimIdentityMap(unsigned rank);
  // One symbol identity map: ()[s] -> (s).
  AffineMap getSymbolIdentityMap();

  /// Returns a map that shifts its (single) input dimension by 'shift'.
  /// (d0) -> (d0 + shift)
  AffineMap getSingleDimShiftAffineMap(int64_t shift);

  /// Returns an affine map that is a translation (shift) of all result
  /// expressions in 'map' by 'shift'.
  /// Eg: input: (d0, d1)[s0] -> (d0, d1 + s0), shift = 2
  ///   returns:    (d0, d1)[s0] -> (d0 + 2, d1 + s0 + 2)
  AffineMap getShiftedAffineMap(AffineMap map, int64_t shift);

  // Integer set.
  IntegerSet getIntegerSet(unsigned dimCount, unsigned symbolCount,
                           ArrayRef<AffineExpr> constraints,
                           ArrayRef<bool> isEq);
  // TODO: Helpers for affine map/exprs, etc.
protected:
  MLIRContext *context;
};

/// This class helps build a CFGFunction.  Instructions that are created are
/// automatically inserted at an insertion point or added to the current basic
/// block.
class CFGFuncBuilder : public Builder {
public:
  CFGFuncBuilder(BasicBlock *block, BasicBlock::iterator insertPoint)
      : Builder(block->getFunction()->getContext()),
        function(block->getFunction()) {
    setInsertionPoint(block, insertPoint);
  }

  CFGFuncBuilder(OperationInst *insertBefore)
      : CFGFuncBuilder(insertBefore->getBlock(),
                       BasicBlock::iterator(insertBefore)) {}

  CFGFuncBuilder(BasicBlock *block)
      : Builder(block->getFunction()->getContext()),
        function(block->getFunction()) {
    setInsertionPoint(block);
  }

  CFGFuncBuilder(CFGFunction *function)
      : Builder(function->getContext()), function(function) {}

  /// Return the function this builder is referring to.
  CFGFunction *getFunction() const { return function; }

  /// Reset the insertion point to no location.  Creating an operation without a
  /// set insertion point is an error, but this can still be useful when the
  /// current insertion point a builder refers to is being removed.
  void clearInsertionPoint() {
    this->block = nullptr;
    insertPoint = BasicBlock::iterator();
  }

  /// Set the insertion point to the specified location.
  void setInsertionPoint(BasicBlock *block, BasicBlock::iterator insertPoint) {
    assert(block->getFunction() == function &&
           "can't move to a different function");
    this->block = block;
    this->insertPoint = insertPoint;
  }

  /// Set the insertion point to the specified operation.
  void setInsertionPoint(OperationInst *inst) {
    setInsertionPoint(inst->getBlock(), BasicBlock::iterator(inst));
  }

  /// Set the insertion point to the end of the specified block.
  void setInsertionPoint(BasicBlock *block) {
    setInsertionPoint(block, block->end());
  }

  void insert(OperationInst *opInst) {
    block->getOperations().insert(insertPoint, opInst);
  }

  /// Add new basic block and set the insertion point to the end of it.  If an
  /// 'insertBefore' basic block is passed, the block will be placed before the
  /// specified block.  If not, the block will be appended to the end of the
  /// current function.
  BasicBlock *createBlock(BasicBlock *insertBefore = nullptr);

  /// Create an operation given the fields represented as an OperationState.
  OperationInst *createOperation(const OperationState &state);

  /// Create operation of specific op type at the current insertion point
  /// without verifying to see if it is valid.
  template <typename OpTy, typename... Args>
  OpPointer<OpTy> create(Location *location, Args... args) {
    OperationState state(getContext(), location, OpTy::getOperationName());
    OpTy::build(this, &state, args...);
    auto *inst = createOperation(state);
    auto result = inst->dyn_cast<OpTy>();
    assert(result && "Builder didn't return the right type");
    return result;
  }

  /// Creates an operation of specific op type at the current insertion point.
  /// If the result is an invalid op (the verifier hook fails), emit an error
  /// and return null.
  template <typename OpTy, typename... Args>
  OpPointer<OpTy> createChecked(Location *location, Args... args) {
    OperationState state(getContext(), location, OpTy::getOperationName());
    OpTy::build(this, &state, args...);
    auto *inst = createOperation(state);

    // If the OperationInst we produce is valid, return it.
    if (!OpTy::verifyInvariants(inst)) {
      auto result = inst->dyn_cast<OpTy>();
      assert(result && "Builder didn't return the right type");
      return result;
    }

    // Otherwise, the error message got emitted.  Just remove the instruction
    // we made.
    inst->erase();
    return OpPointer<OpTy>();
  }

  OperationInst *cloneOperation(const OperationInst &srcOpInst) {
    auto *op = srcOpInst.clone();
    insert(op);
    return op;
  }

  // Terminators.

  ReturnInst *createReturn(Location *location, ArrayRef<CFGValue *> operands) {
    return insertTerminator(ReturnInst::create(location, operands));
  }

  BranchInst *createBranch(Location *location, BasicBlock *dest,
                           ArrayRef<CFGValue *> operands = {}) {
    return insertTerminator(BranchInst::create(location, dest, operands));
  }

  CondBranchInst *createCondBranch(Location *location, CFGValue *condition,
                                   BasicBlock *trueDest,
                                   BasicBlock *falseDest) {
    return insertTerminator(
        CondBranchInst::create(location, condition, trueDest, falseDest));
  }

private:
  template <typename T> T *insertTerminator(T *term) {
    block->setTerminator(term);
    return term;
  }

  CFGFunction *function;
  BasicBlock *block = nullptr;
  BasicBlock::iterator insertPoint;
};

/// This class helps build an MLFunction.  Statements that are created are
/// automatically inserted at an insertion point or added to the current
/// statement block. The builder has only two member variables and can be passed
/// around by value.
class MLFuncBuilder : public Builder {
public:
  /// Create ML function builder and set insertion point to the given statement,
  /// which will cause subsequent insertions to go right before it.
  MLFuncBuilder(Statement *stmt)
      // TODO: Eliminate findFunction from this.
      : MLFuncBuilder(stmt->findFunction()) {
    setInsertionPoint(stmt);
  }

  MLFuncBuilder(StmtBlock *block, StmtBlock::iterator insertPoint)
      // TODO: Eliminate findFunction from this.
      : MLFuncBuilder(block->findFunction()) {
    setInsertionPoint(block, insertPoint);
  }

  /// Create an ML function builder and set the insertion point to the start of
  /// the function.
  MLFuncBuilder(MLFunction *func)
      : Builder(func->getContext()), function(func) {
    setInsertionPoint(func, func->begin());
  }

  /// Return the function this builder is referring to.
  MLFunction *getFunction() const { return function; }

  /// Reset the insertion point to no location.  Creating an operation without a
  /// set insertion point is an error, but this can still be useful when the
  /// current insertion point a builder refers to is being removed.
  void clearInsertionPoint() {
    this->block = nullptr;
    insertPoint = StmtBlock::iterator();
  }

  /// Set the insertion point to the specified location.
  /// Unlike CFGFuncBuilder, MLFuncBuilder allows to set insertion
  /// point to a different function.
  void setInsertionPoint(StmtBlock *block, StmtBlock::iterator insertPoint) {
    // TODO: check that insertPoint is in this rather than some other block.
    this->block = block;
    this->insertPoint = insertPoint;
  }

  /// Sets the insertion point to the specified operation, which will cause
  /// subsequent insertions to go right before it.
  void setInsertionPoint(Statement *stmt) {
    setInsertionPoint(stmt->getBlock(), StmtBlock::iterator(stmt));
  }

  /// Sets the insertion point to the start of the specified block.
  void setInsertionPointToStart(StmtBlock *block) {
    setInsertionPoint(block, block->begin());
  }

  /// Sets the insertion point to the end of the specified block.
  void setInsertionPointToEnd(StmtBlock *block) {
    setInsertionPoint(block, block->end());
  }

  /// Returns a builder for the body of a for Stmt.
  static MLFuncBuilder getForStmtBodyBuilder(ForStmt *forStmt) {
    return MLFuncBuilder(forStmt, forStmt->end());
  }

  /// Returns the current insertion point of the builder.
  StmtBlock::iterator getInsertionPoint() const { return insertPoint; }

  /// Returns the current block of the builder.
  StmtBlock *getBlock() const { return block; }

  /// Creates an operation given the fields represented as an OperationState.
  OperationStmt *createOperation(const OperationState &state);

  /// Creates an operation given the fields.
  OperationStmt *createOperation(Location *location, OperationName name,
                                 ArrayRef<MLValue *> operands,
                                 ArrayRef<Type> types,
                                 ArrayRef<NamedAttribute> attrs);

  /// Create operation of specific op type at the current insertion point.
  template <typename OpTy, typename... Args>
  OpPointer<OpTy> create(Location *location, Args... args) {
    OperationState state(getContext(), location, OpTy::getOperationName());
    OpTy::build(this, &state, args...);
    auto *stmt = createOperation(state);
    auto result = stmt->dyn_cast<OpTy>();
    assert(result && "Builder didn't return the right type");
    return result;
  }

  /// Creates an operation of specific op type at the current insertion point.
  /// If the result is an invalid op (the verifier hook fails), emit an error
  /// and return null.
  template <typename OpTy, typename... Args>
  OpPointer<OpTy> createChecked(Location *location, Args... args) {
    OperationState state(getContext(), location, OpTy::getOperationName());
    OpTy::build(this, &state, args...);
    auto *stmt = createOperation(state);

    // If the OperationStmt we produce is valid, return it.
    if (!OpTy::verifyInvariants(stmt)) {
      auto result = stmt->dyn_cast<OpTy>();
      assert(result && "Builder didn't return the right type");
      return result;
    }

    // Otherwise, the error message got emitted.  Just remove the statement
    // we made.
    stmt->erase();
    return OpPointer<OpTy>();
  }

  /// Creates a deep copy of the specified statement, remapping any operands
  /// that use values outside of the statement using the map that is provided (
  /// leaving them alone if no entry is present).  Replaces references to cloned
  /// sub-statements to the corresponding statement that is copied, and adds
  /// those mappings to the map.
  Statement *clone(const Statement &stmt,
                   OperationStmt::OperandMapTy &operandMapping) {
    Statement *cloneStmt = stmt.clone(operandMapping, getContext());
    block->getStatements().insert(insertPoint, cloneStmt);
    return cloneStmt;
  }

  // Creates a for statement. When step is not specified, it is set to 1.
  ForStmt *createFor(Location *location, ArrayRef<MLValue *> lbOperands,
                     AffineMap lbMap, ArrayRef<MLValue *> ubOperands,
                     AffineMap ubMap, int64_t step = 1);

  // Creates a for statement with known (constant) lower and upper bounds.
  // Default step is 1.
  ForStmt *createFor(Location *loc, int64_t lb, int64_t ub, int64_t step = 1);

  /// Creates if statement.
  IfStmt *createIf(Location *location, ArrayRef<MLValue *> operands,
                   IntegerSet set);

private:
  MLFunction *function;
  StmtBlock *block = nullptr;
  StmtBlock::iterator insertPoint;
};

// Wrapper around common CFGFuncBuilder and MLFuncBuilder functionality. Use
// this wrapper for interfaces where operations need to be created in either a
// CFG function or ML function.
class FuncBuilder : public Builder {
public:
  FuncBuilder(CFGFuncBuilder &cfgFuncBuilder)
      : Builder(cfgFuncBuilder.getContext()), cfgFuncBuilder(&cfgFuncBuilder),
        mlFuncBuilder(nullptr) {}
  FuncBuilder(MLFuncBuilder &mlFuncBuilder)
      : Builder(mlFuncBuilder.getContext()), cfgFuncBuilder(nullptr),
        mlFuncBuilder(&mlFuncBuilder) {}

  /// Creates an operation given the fields represented as an OperationState.
  Operation *createOperation(const OperationState &state) {
    if (cfgFuncBuilder)
      return cfgFuncBuilder->createOperation(state);
    return mlFuncBuilder->createOperation(state);
  }

  /// Creates operation of specific op type at the current insertion point
  /// without verifying to see if it is valid.
  template <typename OpTy, typename... Args>
  OpPointer<OpTy> create(Location *location, Args... args) {
    if (cfgFuncBuilder)
      return cfgFuncBuilder->create<OpTy, Args...>(location, args...);
    return mlFuncBuilder->create<OpTy, Args...>(location, args...);
  }

  /// Creates an operation of specific op type at the current insertion point.
  /// If the result is an invalid op (the verifier hook fails), emit an error
  /// and return null.
  template <typename OpTy, typename... Args>
  OpPointer<OpTy> createChecked(Location *location, Args... args) {
    if (cfgFuncBuilder)
      return cfgFuncBuilder->createChecked<OpTy, Args...>(location, args...);
    return mlFuncBuilder->createChecked<OpTy, Args...>(location, args...);
  }

  /// Set the insertion point to the specified operation. This requires that the
  /// input operation is a OperationInst when building a CFG function and a
  /// OperationStmt when building a ML function.
  void setInsertionPoint(Operation *op) {
    if (cfgFuncBuilder)
      cfgFuncBuilder->setInsertionPoint(cast<OperationInst>(op));
    else
      mlFuncBuilder->setInsertionPoint(cast<OperationStmt>(op));
  }

private:
  // Wrapped builders for CFG and ML functions. Exactly one of these should be
  // non-null.
  CFGFuncBuilder *const cfgFuncBuilder;
  MLFuncBuilder *const mlFuncBuilder;
};

} // namespace mlir

#endif
