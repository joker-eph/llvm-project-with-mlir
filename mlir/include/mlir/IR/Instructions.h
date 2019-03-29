//===- Instructions.h - MLIR ML Instruction Classes -----------------*- C++
//-*-===//
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
// This file defines classes for special kinds of ML Function instructions.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_IR_INSTRUCTIONS_H
#define MLIR_IR_INSTRUCTIONS_H

#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/Instruction.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/OperationSupport.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/TrailingObjects.h"

namespace mlir {
class AffineBound;
class IntegerSet;
class AffineCondition;
class AttributeListStorage;
template <typename OpType> class ConstOpPointer;
template <typename OpType> class OpPointer;
template <typename ObjectType, typename ElementType> class ResultIterator;
template <typename ObjectType, typename ElementType> class ResultTypeIterator;
class Function;

/// Operations represent all of the arithmetic and other basic computation in
/// MLIR.
///
class OperationInst final
    : public Instruction,
      private llvm::TrailingObjects<OperationInst, InstResult, BlockOperand,
                                    unsigned, InstOperand> {
public:
  /// Create a new OperationInst with the specific fields.
  static OperationInst *
  create(Location location, OperationName name, ArrayRef<Value *> operands,
         ArrayRef<Type> resultTypes, ArrayRef<NamedAttribute> attributes,
         ArrayRef<Block *> successors, MLIRContext *context);

  /// Return the context this operation is associated with.
  MLIRContext *getContext() const;

  /// The name of an operation is the key identifier for it.
  OperationName getName() const { return name; }

  /// If this operation has a registered operation description, return it.
  /// Otherwise return null.
  const AbstractOperation *getAbstractOperation() const {
    return getName().getAbstractOperation();
  }

  /// Check if this instruction is a return instruction.
  bool isReturn() const;

  //===--------------------------------------------------------------------===//
  // Operands
  //===--------------------------------------------------------------------===//

  unsigned getNumOperands() const { return numOperands; }

  Value *getOperand(unsigned idx) { return getInstOperand(idx).get(); }
  const Value *getOperand(unsigned idx) const {
    return getInstOperand(idx).get();
  }
  void setOperand(unsigned idx, Value *value) {
    return getInstOperand(idx).set(value);
  }

  // Support non-const operand iteration.
  using operand_iterator = OperandIterator<OperationInst, Value>;

  operand_iterator operand_begin() { return operand_iterator(this, 0); }

  operand_iterator operand_end() {
    return operand_iterator(this, getNumOperands());
  }

  /// Returns an iterator on the underlying Value's (Value *).
  llvm::iterator_range<operand_iterator> getOperands() {
    return {operand_begin(), operand_end()};
  }

  // Support const operand iteration.
  using const_operand_iterator =
      OperandIterator<const OperationInst, const Value>;

  const_operand_iterator operand_begin() const {
    return const_operand_iterator(this, 0);
  }

  const_operand_iterator operand_end() const {
    return const_operand_iterator(this, getNumOperands());
  }

  /// Returns a const iterator on the underlying Value's (Value *).
  llvm::iterator_range<const_operand_iterator> getOperands() const {
    return {operand_begin(), operand_end()};
  }

  ArrayRef<InstOperand> getInstOperands() const {
    return {getTrailingObjects<InstOperand>(), numOperands};
  }
  MutableArrayRef<InstOperand> getInstOperands() {
    return {getTrailingObjects<InstOperand>(), numOperands};
  }

  InstOperand &getInstOperand(unsigned idx) { return getInstOperands()[idx]; }
  const InstOperand &getInstOperand(unsigned idx) const {
    return getInstOperands()[idx];
  }

  //===--------------------------------------------------------------------===//
  // Results
  //===--------------------------------------------------------------------===//

  /// Return true if there are no users of any results of this operation.
  bool use_empty() const;

  unsigned getNumResults() const { return numResults; }

  Value *getResult(unsigned idx) { return &getInstResult(idx); }
  const Value *getResult(unsigned idx) const { return &getInstResult(idx); }

  // Support non-const result iteration.
  using result_iterator = ResultIterator<OperationInst, Value>;
  result_iterator result_begin();
  result_iterator result_end();
  llvm::iterator_range<result_iterator> getResults();

  // Support const result iteration.
  using const_result_iterator =
      ResultIterator<const OperationInst, const Value>;
  const_result_iterator result_begin() const;

  const_result_iterator result_end() const;

  llvm::iterator_range<const_result_iterator> getResults() const;

  ArrayRef<InstResult> getInstResults() const {
    return {getTrailingObjects<InstResult>(), numResults};
  }

  MutableArrayRef<InstResult> getInstResults() {
    return {getTrailingObjects<InstResult>(), numResults};
  }

  InstResult &getInstResult(unsigned idx) { return getInstResults()[idx]; }

  const InstResult &getInstResult(unsigned idx) const {
    return getInstResults()[idx];
  }

  // Support result type iteration.
  using result_type_iterator =
      ResultTypeIterator<const OperationInst, const Value>;
  result_type_iterator result_type_begin() const;

  result_type_iterator result_type_end() const;

  llvm::iterator_range<result_type_iterator> getResultTypes() const;

  //===--------------------------------------------------------------------===//
  // Attributes
  //===--------------------------------------------------------------------===//

  // Operations may optionally carry a list of attributes that associate
  // constants to names.  Attributes may be dynamically added and removed over
  // the lifetime of an operation.
  //
  // We assume there will be relatively few attributes on a given operation
  // (maybe a dozen or so, but not hundreds or thousands) so we use linear
  // searches for everything.

  /// Return all of the attributes on this operation.
  ArrayRef<NamedAttribute> getAttrs() const;

  /// Return the specified attribute if present, null otherwise.
  Attribute getAttr(Identifier name) const {
    for (auto elt : getAttrs())
      if (elt.first == name)
        return elt.second;
    return nullptr;
  }

  Attribute getAttr(StringRef name) const {
    for (auto elt : getAttrs())
      if (elt.first.is(name))
        return elt.second;
    return nullptr;
  }

  template <typename AttrClass> AttrClass getAttrOfType(Identifier name) const {
    return getAttr(name).dyn_cast_or_null<AttrClass>();
  }

  template <typename AttrClass> AttrClass getAttrOfType(StringRef name) const {
    return getAttr(name).dyn_cast_or_null<AttrClass>();
  }

  /// If the an attribute exists with the specified name, change it to the new
  /// value.  Otherwise, add a new attribute with the specified name/value.
  void setAttr(Identifier name, Attribute value);

  enum class RemoveResult { Removed, NotFound };

  /// Remove the attribute with the specified name if it exists.  The return
  /// value indicates whether the attribute was present or not.
  RemoveResult removeAttr(Identifier name);

  //===--------------------------------------------------------------------===//
  // Terminators
  //===--------------------------------------------------------------------===//

  MutableArrayRef<BlockOperand> getBlockOperands() {
    assert(isTerminator() && "Only terminators have a block operands list");
    return {getTrailingObjects<BlockOperand>(), numSuccs};
  }
  ArrayRef<BlockOperand> getBlockOperands() const {
    return const_cast<OperationInst *>(this)->getBlockOperands();
  }

  llvm::iterator_range<const_operand_iterator>
  getSuccessorOperands(unsigned index) const;
  llvm::iterator_range<operand_iterator> getSuccessorOperands(unsigned index);

  unsigned getNumSuccessors() const { return numSuccs; }
  unsigned getNumSuccessorOperands(unsigned index) const {
    assert(isTerminator() && "Only terminators have successors");
    assert(index < getNumSuccessors());
    return getTrailingObjects<unsigned>()[index];
  }

  Block *getSuccessor(unsigned index) {
    assert(index < getNumSuccessors());
    return getBlockOperands()[index].get();
  }
  const Block *getSuccessor(unsigned index) const {
    return const_cast<OperationInst *>(this)->getSuccessor(index);
  }
  void setSuccessor(Block *block, unsigned index);

  /// Erase a specific operand from the operand list of the successor at
  /// 'index'.
  void eraseSuccessorOperand(unsigned succIndex, unsigned opIndex) {
    assert(succIndex < getNumSuccessors());
    assert(opIndex < getNumSuccessorOperands(succIndex));
    eraseOperand(getSuccessorOperandIndex(succIndex) + opIndex);
    --getTrailingObjects<unsigned>()[succIndex];
  }

  /// Get the index of the first operand of the successor at the provided
  /// index.
  unsigned getSuccessorOperandIndex(unsigned index) const {
    assert(isTerminator() && "Only terminators have successors.");
    assert(index < getNumSuccessors());

    // Count the number of operands for each of the successors after, and
    // including, the one at 'index'. This is based upon the assumption that all
    // non successor operands are placed at the beginning of the operand list.
    auto *successorOpCountBegin = getTrailingObjects<unsigned>();
    unsigned postSuccessorOpCount =
        std::accumulate(successorOpCountBegin + index,
                        successorOpCountBegin + getNumSuccessors(), 0);
    return getNumOperands() - postSuccessorOpCount;
  }

  //===--------------------------------------------------------------------===//
  // Accessors for various properties of operations
  //===--------------------------------------------------------------------===//

  /// Returns whether the operation is commutative.
  bool isCommutative() const {
    if (auto *absOp = getAbstractOperation())
      return absOp->hasProperty(OperationProperty::Commutative);
    return false;
  }

  /// Returns whether the operation has side-effects.
  bool hasNoSideEffect() const {
    if (auto *absOp = getAbstractOperation())
      return absOp->hasProperty(OperationProperty::NoSideEffect);
    return false;
  }

  /// Returns whether the operation is a terminator.
  bool isTerminator() const {
    if (auto *absOp = getAbstractOperation())
      return absOp->hasProperty(OperationProperty::Terminator);
    return false;
  }

  /// Attempt to constant fold this operation with the specified constant
  /// operand values - the elements in "operands" will correspond directly to
  /// the operands of the operation, but may be null if non-constant.  If
  /// constant folding is successful, this returns false and fills in the
  /// `results` vector.  If not, this returns true and `results` is unspecified.
  bool constantFold(ArrayRef<Attribute> operands,
                    SmallVectorImpl<Attribute> &results) const;

  //===--------------------------------------------------------------------===//
  // Conversions to declared operations like DimOp
  //===--------------------------------------------------------------------===//

  // Return a null OpPointer for the specified type.
  template <typename OpClass> static OpPointer<OpClass> getNull() {
    return OpPointer<OpClass>(OpClass(nullptr));
  }

  /// The dyn_cast methods perform a dynamic cast from an OperationInst (like
  /// Instruction and OperationInst) to a typed Op like DimOp.  This returns
  /// a null OpPointer on failure.
  template <typename OpClass> OpPointer<OpClass> dyn_cast() {
    if (isa<OpClass>()) {
      return cast<OpClass>();
    } else {
      return OpPointer<OpClass>(OpClass(nullptr));
    }
  }

  /// The dyn_cast methods perform a dynamic cast from an OperationInst (like
  /// Instruction and OperationInst) to a typed Op like DimOp.  This returns
  /// a null ConstOpPointer on failure.
  template <typename OpClass> ConstOpPointer<OpClass> dyn_cast() const {
    if (isa<OpClass>()) {
      return cast<OpClass>();
    } else {
      return ConstOpPointer<OpClass>(OpClass(nullptr));
    }
  }

  /// The cast methods perform a cast from an OperationInst (like
  /// Instruction and OperationInst) to a typed Op like DimOp.  This aborts
  /// if the parameter to the template isn't an instance of the template type
  /// argument.
  template <typename OpClass> OpPointer<OpClass> cast() {
    assert(isa<OpClass>() && "cast<Ty>() argument of incompatible type!");
    return OpPointer<OpClass>(OpClass(this));
  }

  /// The cast methods perform a cast from an OperationInst (like
  /// Instruction and OperationInst) to a typed Op like DimOp.  This aborts
  /// if the parameter to the template isn't an instance of the template type
  /// argument.
  template <typename OpClass> ConstOpPointer<OpClass> cast() const {
    assert(isa<OpClass>() && "cast<Ty>() argument of incompatible type!");
    return ConstOpPointer<OpClass>(OpClass(this));
  }

  /// The is methods return true if the operation is a typed op (like DimOp) of
  /// of the given class.
  template <typename OpClass> bool isa() const {
    return OpClass::isClassFor(this);
  }

  //===--------------------------------------------------------------------===//
  // Other
  //===--------------------------------------------------------------------===//

  /// Emit an error with the op name prefixed, like "'dim' op " which is
  /// convenient for verifiers.  This function always returns true.
  bool emitOpError(const Twine &message) const;

  void destroy();

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const IROperandOwner *ptr) {
    return ptr->getKind() == IROperandOwner::Kind::OperationInst;
  }

private:
  unsigned numOperands;
  const unsigned numResults, numSuccs;

  /// This holds the name of the operation.
  OperationName name;

  /// This holds general named attributes for the operation.
  AttributeListStorage *attrs;

  OperationInst(Location location, OperationName name, unsigned numOperands,
                unsigned numResults, unsigned numSuccessors,
                ArrayRef<NamedAttribute> attributes, MLIRContext *context);
  ~OperationInst();

  /// Erase the operand at 'index'.
  void eraseOperand(unsigned index);

  // This stuff is used by the TrailingObjects template.
  friend llvm::TrailingObjects<OperationInst, InstResult, BlockOperand,
                               unsigned, InstOperand>;
  size_t numTrailingObjects(OverloadToken<InstOperand>) const {
    return numOperands;
  }
  size_t numTrailingObjects(OverloadToken<InstResult>) const {
    return numResults;
  }
  size_t numTrailingObjects(OverloadToken<BlockOperand>) const {
    return numSuccs;
  }
  size_t numTrailingObjects(OverloadToken<unsigned>) const { return numSuccs; }
};

/// This template implements the result iterators for the OperationInst class
/// in terms of getResult(idx).
template <typename ObjectType, typename ElementType>
class ResultIterator final
    : public IndexedAccessorIterator<ResultIterator<ObjectType, ElementType>,
                                     ObjectType, ElementType> {
public:
  /// Initializes the result iterator to the specified index.
  ResultIterator(ObjectType *object, unsigned index)
      : IndexedAccessorIterator<ResultIterator<ObjectType, ElementType>,
                                ObjectType, ElementType>(object, index) {}

  /// Support converting to the const variant. This will be a no-op for const
  /// variant.
  operator ResultIterator<const ObjectType, const ElementType>() const {
    return ResultIterator<const ObjectType, const ElementType>(this->object,
                                                               this->index);
  }

  ElementType *operator*() const {
    return this->object->getResult(this->index);
  }
};

/// This template implements the result type iterators for the OperationInst
/// class in terms of getResult(idx)->getType().
template <typename ObjectType, typename ElementType>
class ResultTypeIterator final
    : public IndexedAccessorIterator<
          ResultTypeIterator<ObjectType, ElementType>, ObjectType,
          ElementType> {
public:
  /// Initializes the result type iterator to the specified index.
  ResultTypeIterator(ObjectType *object, unsigned index)
      : IndexedAccessorIterator<ResultTypeIterator<ObjectType, ElementType>,
                                ObjectType, ElementType>(object, index) {}

  /// Support converting to the const variant. This will be a no-op for const
  /// variant.
  operator ResultTypeIterator<const ObjectType, const ElementType>() const {
    return ResultTypeIterator<const ObjectType, const ElementType>(this->object,
                                                                   this->index);
  }

  Type operator*() const {
    return this->object->getResult(this->index)->getType();
  }
};

// Implement the inline result iterator methods.
inline auto OperationInst::result_begin() -> result_iterator {
  return result_iterator(this, 0);
}

inline auto OperationInst::result_end() -> result_iterator {
  return result_iterator(this, getNumResults());
}

inline auto OperationInst::getResults()
    -> llvm::iterator_range<result_iterator> {
  return {result_begin(), result_end()};
}

inline auto OperationInst::result_begin() const -> const_result_iterator {
  return const_result_iterator(this, 0);
}

inline auto OperationInst::result_end() const -> const_result_iterator {
  return const_result_iterator(this, getNumResults());
}

inline auto OperationInst::getResults() const
    -> llvm::iterator_range<const_result_iterator> {
  return {result_begin(), result_end()};
}

inline auto OperationInst::result_type_begin() const -> result_type_iterator {
  return result_type_iterator(this, 0);
}

inline auto OperationInst::result_type_end() const -> result_type_iterator {
  return result_type_iterator(this, getNumResults());
}

inline auto OperationInst::getResultTypes() const
    -> llvm::iterator_range<result_type_iterator> {
  return {result_type_begin(), result_type_end()};
}

/// For instruction represents an affine loop nest.
class ForInst : public Instruction, public Value {
public:
  static ForInst *create(Location location, ArrayRef<Value *> lbOperands,
                         AffineMap lbMap, ArrayRef<Value *> ubOperands,
                         AffineMap ubMap, int64_t step);

  ~ForInst() {
    // There may be references to the induction variable of this loop within its
    // body or, in case of ill-formed code during parsing, outside its body.
    // Explicitly drop all uses of the induction variable before destroying it.
    dropAllUses();
  }

  /// Resolve base class ambiguity.
  using Instruction::getFunction;

  /// Operand iterators.
  using operand_iterator = OperandIterator<ForInst, Value>;
  using const_operand_iterator = OperandIterator<const ForInst, const Value>;

  /// Operand iterator range.
  using operand_range = llvm::iterator_range<operand_iterator>;
  using const_operand_range = llvm::iterator_range<const_operand_iterator>;

  /// Get the body of the ForInst.
  Block *getBody() { return &body.front(); }

  /// Get the body of the ForInst.
  const Block *getBody() const { return &body.front(); }

  //===--------------------------------------------------------------------===//
  // Bounds and step
  //===--------------------------------------------------------------------===//

  /// Returns information about the lower bound as a single object.
  const AffineBound getLowerBound() const;

  /// Returns information about the upper bound as a single object.
  const AffineBound getUpperBound() const;

  /// Returns loop step.
  int64_t getStep() const { return step; }

  /// Returns affine map for the lower bound.
  AffineMap getLowerBoundMap() const { return lbMap; }
  /// Returns affine map for the upper bound. The upper bound is exclusive.
  AffineMap getUpperBoundMap() const { return ubMap; }

  /// Set lower bound.
  void setLowerBound(ArrayRef<Value *> operands, AffineMap map);
  /// Set upper bound.
  void setUpperBound(ArrayRef<Value *> operands, AffineMap map);

  /// Set the lower bound map without changing operands.
  void setLowerBoundMap(AffineMap map);

  /// Set the upper bound map without changing operands.
  void setUpperBoundMap(AffineMap map);

  /// Set loop step.
  void setStep(int64_t step) {
    assert(step > 0 && "step has to be a positive integer constant");
    this->step = step;
  }

  /// Returns true if the lower bound is constant.
  bool hasConstantLowerBound() const;
  /// Returns true if the upper bound is constant.
  bool hasConstantUpperBound() const;
  /// Returns true if both bounds are constant.
  bool hasConstantBounds() const {
    return hasConstantLowerBound() && hasConstantUpperBound();
  }
  /// Returns the value of the constant lower bound.
  /// Fails assertion if the bound is non-constant.
  int64_t getConstantLowerBound() const;
  /// Returns the value of the constant upper bound. The upper bound is
  /// exclusive. Fails assertion if the bound is non-constant.
  int64_t getConstantUpperBound() const;
  /// Sets the lower bound to the given constant value.
  void setConstantLowerBound(int64_t value);
  /// Sets the upper bound to the given constant value.
  void setConstantUpperBound(int64_t value);

  /// Returns true if both the lower and upper bound have the same operand lists
  /// (same operands in the same order).
  bool matchingBoundOperandList() const;

  //===--------------------------------------------------------------------===//
  // Operands
  //===--------------------------------------------------------------------===//

  unsigned getNumOperands() const { return operands.size(); }

  Value *getOperand(unsigned idx) { return getInstOperand(idx).get(); }
  const Value *getOperand(unsigned idx) const {
    return getInstOperand(idx).get();
  }
  void setOperand(unsigned idx, Value *value) {
    getInstOperand(idx).set(value);
  }

  operand_iterator operand_begin() { return operand_iterator(this, 0); }
  operand_iterator operand_end() {
    return operand_iterator(this, getNumOperands());
  }

  const_operand_iterator operand_begin() const {
    return const_operand_iterator(this, 0);
  }
  const_operand_iterator operand_end() const {
    return const_operand_iterator(this, getNumOperands());
  }

  ArrayRef<InstOperand> getInstOperands() const { return operands; }
  MutableArrayRef<InstOperand> getInstOperands() { return operands; }
  InstOperand &getInstOperand(unsigned idx) { return getInstOperands()[idx]; }
  const InstOperand &getInstOperand(unsigned idx) const {
    return getInstOperands()[idx];
  }

  // TODO: provide iterators for the lower and upper bound operands
  // if the current access via getLowerBound(), getUpperBound() is too slow.

  /// Returns operands for the lower bound map.
  operand_range getLowerBoundOperands();
  const_operand_range getLowerBoundOperands() const;

  /// Returns operands for the upper bound map.
  operand_range getUpperBoundOperands();
  const_operand_range getUpperBoundOperands() const;

  //===--------------------------------------------------------------------===//
  // Other
  //===--------------------------------------------------------------------===//

  /// Return the context this operation is associated with.
  MLIRContext *getContext() const { return getType().getContext(); }

  using Instruction::dump;
  using Instruction::print;

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const IROperandOwner *ptr) {
    return ptr->getKind() == IROperandOwner::Kind::ForInst;
  }

  // For instruction represents implicitly represents induction variable by
  // inheriting from Value class. Whenever you need to refer to the loop
  // induction variable, just use the for instruction itself.
  static bool classof(const Value *value) {
    return value->getKind() == Value::Kind::ForInst;
  }

private:
  // The Block for the body. By construction, this list always contains exactly
  // one block.
  BlockList body;

  // Affine map for the lower bound.
  AffineMap lbMap;
  // Affine map for the upper bound. The upper bound is exclusive.
  AffineMap ubMap;
  // Positive constant step. Since index is stored as an int64_t, we restrict
  // step to the set of positive integers that int64_t can represent.
  int64_t step;
  // Operands for the lower and upper bounds, with the former followed by the
  // latter. Dimensional operands are followed by symbolic operands for each
  // bound.
  std::vector<InstOperand> operands;

  explicit ForInst(Location location, unsigned numOperands, AffineMap lbMap,
                   AffineMap ubMap, int64_t step);
};

/// AffineBound represents a lower or upper bound in the for instruction.
/// This class does not own the underlying operands. Instead, it refers
/// to the operands stored in the ForInst. Its life span should not exceed
/// that of the for instruction it refers to.
class AffineBound {
public:
  const ForInst *getForInst() const { return &inst; }
  AffineMap getMap() const { return map; }

  unsigned getNumOperands() const { return opEnd - opStart; }
  const Value *getOperand(unsigned idx) const {
    return inst.getOperand(opStart + idx);
  }
  const InstOperand &getInstOperand(unsigned idx) const {
    return inst.getInstOperand(opStart + idx);
  }

  using operand_iterator = ForInst::operand_iterator;
  using operand_range = ForInst::operand_range;

  operand_iterator operand_begin() const {
    // These are iterators over Value *. Not casting away const'ness would
    // require the caller to use const Value *.
    return operand_iterator(const_cast<ForInst *>(&inst), opStart);
  }
  operand_iterator operand_end() const {
    return operand_iterator(const_cast<ForInst *>(&inst), opEnd);
  }

  /// Returns an iterator on the underlying Value's (Value *).
  operand_range getOperands() const { return {operand_begin(), operand_end()}; }
  ArrayRef<InstOperand> getInstOperands() const {
    auto ops = inst.getInstOperands();
    return ArrayRef<InstOperand>(ops.begin() + opStart, ops.begin() + opEnd);
  }

private:
  // 'for' instruction that contains this bound.
  const ForInst &inst;
  // Start and end positions of this affine bound operands in the list of
  // the containing 'for' instruction operands.
  unsigned opStart, opEnd;
  // Affine map for this bound.
  AffineMap map;

  AffineBound(const ForInst &inst, unsigned opStart, unsigned opEnd,
              AffineMap map)
      : inst(inst), opStart(opStart), opEnd(opEnd), map(map) {}

  friend class ForInst;
};

/// If instruction restricts execution to a subset of the loop iteration space.
class IfInst : public Instruction {
public:
  static IfInst *create(Location location, ArrayRef<Value *> operands,
                        IntegerSet set);
  ~IfInst();

  //===--------------------------------------------------------------------===//
  // Then, else, condition.
  //===--------------------------------------------------------------------===//

  Block *getThen() { return &thenClause.front(); }
  const Block *getThen() const { return &thenClause.front(); }
  Block *getElse() { return elseClause ? &elseClause->front() : nullptr; }
  const Block *getElse() const {
    return elseClause ? &elseClause->front() : nullptr;
  }
  bool hasElse() const { return elseClause != nullptr; }

  Block *createElse() {
    assert(elseClause == nullptr && "already has an else clause!");
    elseClause = new BlockList(this);
    elseClause->push_back(new Block());
    return &elseClause->front();
  }

  const AffineCondition getCondition() const;

  IntegerSet getIntegerSet() const { return set; }
  void setIntegerSet(IntegerSet newSet) {
    assert(newSet.getNumOperands() == operands.size());
    set = newSet;
  }

  //===--------------------------------------------------------------------===//
  // Operands
  //===--------------------------------------------------------------------===//

  /// Operand iterators.
  using operand_iterator = OperandIterator<IfInst, Value>;
  using const_operand_iterator = OperandIterator<const IfInst, const Value>;

  /// Operand iterator range.
  using operand_range = llvm::iterator_range<operand_iterator>;
  using const_operand_range = llvm::iterator_range<const_operand_iterator>;

  unsigned getNumOperands() const { return operands.size(); }

  Value *getOperand(unsigned idx) { return getInstOperand(idx).get(); }
  const Value *getOperand(unsigned idx) const {
    return getInstOperand(idx).get();
  }
  void setOperand(unsigned idx, Value *value) {
    getInstOperand(idx).set(value);
  }

  operand_iterator operand_begin() { return operand_iterator(this, 0); }
  operand_iterator operand_end() {
    return operand_iterator(this, getNumOperands());
  }

  const_operand_iterator operand_begin() const {
    return const_operand_iterator(this, 0);
  }
  const_operand_iterator operand_end() const {
    return const_operand_iterator(this, getNumOperands());
  }

  ArrayRef<InstOperand> getInstOperands() const { return operands; }
  MutableArrayRef<InstOperand> getInstOperands() { return operands; }
  InstOperand &getInstOperand(unsigned idx) { return getInstOperands()[idx]; }
  const InstOperand &getInstOperand(unsigned idx) const {
    return getInstOperands()[idx];
  }

  //===--------------------------------------------------------------------===//
  // Other
  //===--------------------------------------------------------------------===//

  MLIRContext *getContext() const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const IROperandOwner *ptr) {
    return ptr->getKind() == IROperandOwner::Kind::IfInst;
  }

private:
  // it is always present.
  BlockList thenClause;
  // 'else' clause of the if instruction. 'nullptr' if there is no else clause.
  BlockList *elseClause;

  // The integer set capturing the conditional guard.
  IntegerSet set;

  // Condition operands.
  std::vector<InstOperand> operands;

  explicit IfInst(Location location, unsigned numOperands, IntegerSet set);
};

/// AffineCondition represents a condition of the 'if' instruction.
/// Its life span should not exceed that of the objects it refers to.
/// AffineCondition does not provide its own methods for iterating over
/// the operands since the iterators of the if instruction accomplish
/// the same purpose.
///
/// AffineCondition is trivially copyable, so it should be passed by value.
class AffineCondition {
public:
  const IfInst *getIfInst() const { return &inst; }
  IntegerSet getIntegerSet() const { return set; }

private:
  // 'if' instruction that contains this affine condition.
  const IfInst &inst;
  // Integer set for this affine condition.
  IntegerSet set;

  AffineCondition(const IfInst &inst, IntegerSet set) : inst(inst), set(set) {}

  friend class IfInst;
};
} // end namespace mlir

#endif // MLIR_IR_INSTRUCTIONS_H
