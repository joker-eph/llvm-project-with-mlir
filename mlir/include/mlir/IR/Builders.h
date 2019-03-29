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

#include "mlir/IR/Function.h"
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
  UnknownLoc getUnknownLoc();
  UniquedFilename getUniquedFilename(StringRef filename);
  FileLineColLoc getFileLineColLoc(UniquedFilename filename, unsigned line,
                                   unsigned column);
  Location getFusedLoc(ArrayRef<Location> locs,
                       Attribute metadata = Attribute());

  // Types.
  FloatType getBF16Type();
  FloatType getF16Type();
  FloatType getF32Type();
  FloatType getF64Type();

  IndexType getIndexType();

  OtherType getTFControlType();
  OtherType getTFStringType();
  OtherType getTFResourceType();
  OtherType getTFVariantType();
  OtherType getTFComplex64Type();
  OtherType getTFComplex128Type();
  OtherType getTFF32REFType();

  IntegerType getI1Type();
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
  IntegerAttr getIntegerAttr(Type type, int64_t value);
  IntegerAttr getIntegerAttr(Type type, const APInt &value);
  FloatAttr getFloatAttr(Type type, double value);
  FloatAttr getFloatAttr(Type type, const APFloat &value);
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

  // Convenience methods for fixed types.
  FloatAttr getF32FloatAttr(float value);
  FloatAttr getF64FloatAttr(double value);
  IntegerAttr getI32IntegerAttr(int32_t value);
  IntegerAttr getI64IntegerAttr(int64_t value);

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

/// This class helps build a Function.  Instructions that are created are
/// automatically inserted at an insertion point.  The builder is copyable.
class FuncBuilder : public Builder {
public:
  /// Create a function builder and set the insertion point to the start of
  /// the function.
  FuncBuilder(Function *func) : Builder(func->getContext()), function(func) {
    if (!func->empty())
      setInsertionPoint(&func->front(), func->front().begin());
    else
      clearInsertionPoint();
  }

  /// Create a function builder and set insertion point to the given statement,
  /// which will cause subsequent insertions to go right before it.
  FuncBuilder(Statement *stmt) : FuncBuilder(stmt->getFunction()) {
    setInsertionPoint(stmt);
  }

  FuncBuilder(StmtBlock *block) : FuncBuilder(block->getFunction()) {
    setInsertionPoint(block, block->end());
  }

  FuncBuilder(StmtBlock *block, StmtBlock::iterator insertPoint)
      : FuncBuilder(block->getFunction()) {
    setInsertionPoint(block, insertPoint);
  }

  /// Return the function this builder is referring to.
  Function *getFunction() const { return function; }

  /// Reset the insertion point to no location.  Creating an operation without a
  /// set insertion point is an error, but this can still be useful when the
  /// current insertion point a builder refers to is being removed.
  void clearInsertionPoint() {
    this->block = nullptr;
    insertPoint = StmtBlock::iterator();
  }

  /// Set the insertion point to the specified location.
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

  /// Return the block the current insertion point belongs to.  Note that the
  /// the insertion point is not necessarily the end of the block.
  BasicBlock *getInsertionBlock() const { return block; }

  /// Returns the current insertion point of the builder.
  StmtBlock::iterator getInsertionPoint() const { return insertPoint; }

  /// Add new block and set the insertion point to the end of it.  If an
  /// 'insertBefore' block is passed, the block will be placed before the
  /// specified block.  If not, the block will be appended to the end of the
  /// current function.
  StmtBlock *createBlock(StmtBlock *insertBefore = nullptr);

  /// Returns a builder for the body of a for Stmt.
  static FuncBuilder getForStmtBodyBuilder(ForStmt *forStmt) {
    return FuncBuilder(forStmt->getBody(), forStmt->getBody()->end());
  }

  /// Returns the current block of the builder.
  StmtBlock *getBlock() const { return block; }

  /// Creates an operation given the fields represented as an OperationState.
  OperationStmt *createOperation(const OperationState &state);

  /// Create operation of specific op type at the current insertion point.
  template <typename OpTy, typename... Args>
  OpPointer<OpTy> create(Location location, Args... args) {
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
  OpPointer<OpTy> createChecked(Location location, Args... args) {
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
  ForStmt *createFor(Location location, ArrayRef<Value *> lbOperands,
                     AffineMap lbMap, ArrayRef<Value *> ubOperands,
                     AffineMap ubMap, int64_t step = 1);

  // Creates a for statement with known (constant) lower and upper bounds.
  // Default step is 1.
  ForStmt *createFor(Location loc, int64_t lb, int64_t ub, int64_t step = 1);

  /// Creates if statement.
  IfStmt *createIf(Location location, ArrayRef<Value *> operands,
                   IntegerSet set);

private:
  Function *function;
  StmtBlock *block = nullptr;
  StmtBlock::iterator insertPoint;
};

} // namespace mlir

#endif
