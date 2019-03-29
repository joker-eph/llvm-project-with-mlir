//===- Statement.cpp - MLIR Statement Classes ----------------------------===//
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

#include "AttributeListStorage.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Statements.h"
#include "mlir/IR/StmtVisitor.h"
#include "llvm/ADT/DenseMap.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// InstResult
//===----------------------------------------------------------------------===//

/// Return the result number of this result.
unsigned InstResult::getResultNumber() const {
  // Results are always stored consecutively, so use pointer subtraction to
  // figure out what number this is.
  return this - &getOwner()->getInstResults()[0];
}

//===----------------------------------------------------------------------===//
// InstOperand
//===----------------------------------------------------------------------===//

/// Return which operand this is in the operand list.
template <> unsigned InstOperand::getOperandNumber() const {
  return this - &getOwner()->getInstOperands()[0];
}

/// Return which operand this is in the operand list.
template <> unsigned StmtBlockOperand::getOperandNumber() const {
  return this - &getOwner()->getBlockOperands()[0];
}

//===----------------------------------------------------------------------===//
// Statement
//===----------------------------------------------------------------------===//

// Statements are deleted through the destroy() member because we don't have
// a virtual destructor.
Statement::~Statement() {
  assert(block == nullptr && "statement destroyed but still in a block");
}

/// Destroy this statement or one of its subclasses.
void Statement::destroy() {
  switch (this->getKind()) {
  case Kind::OperationInst:
    cast<OperationInst>(this)->destroy();
    break;
  case Kind::For:
    delete cast<ForStmt>(this);
    break;
  case Kind::If:
    delete cast<IfStmt>(this);
    break;
  }
}

Statement *Statement::getParentStmt() const {
  return block ? block->getContainingStmt() : nullptr;
}

Function *Statement::getFunction() const {
  return block ? block->getFunction() : nullptr;
}

Value *Statement::getOperand(unsigned idx) { return getInstOperand(idx).get(); }

const Value *Statement::getOperand(unsigned idx) const {
  return getInstOperand(idx).get();
}

// Value can be used as a dimension id if it is valid as a symbol, or
// it is an induction variable, or it is a result of affine apply operation
// with dimension id arguments.
bool Value::isValidDim() const {
  if (auto *stmt = getDefiningInst()) {
    // Top level statement or constant operation is ok.
    if (stmt->getParentStmt() == nullptr || stmt->isa<ConstantOp>())
      return true;
    // Affine apply operation is ok if all of its operands are ok.
    if (auto op = stmt->dyn_cast<AffineApplyOp>())
      return op->isValidDim();
    return false;
  }
  // This value is either a function argument or an induction variable. Both
  // are ok.
  return true;
}

// Value can be used as a symbol if it is a constant, or it is defined at
// the top level, or it is a result of affine apply operation with symbol
// arguments.
bool Value::isValidSymbol() const {
  if (auto *stmt = getDefiningInst()) {
    // Top level statement or constant operation is ok.
    if (stmt->getParentStmt() == nullptr || stmt->isa<ConstantOp>())
      return true;
    // Affine apply operation is ok if all of its operands are ok.
    if (auto op = stmt->dyn_cast<AffineApplyOp>())
      return op->isValidSymbol();
    return false;
  }
  // This value is either a function argument or an induction variable.
  // Function argument is ok, induction variable is not.
  return isa<BlockArgument>(this);
}

void Statement::setOperand(unsigned idx, Value *value) {
  getInstOperand(idx).set(value);
}

unsigned Statement::getNumOperands() const {
  switch (getKind()) {
  case Kind::OperationInst:
    return cast<OperationInst>(this)->getNumOperands();
  case Kind::For:
    return cast<ForStmt>(this)->getNumOperands();
  case Kind::If:
    return cast<IfStmt>(this)->getNumOperands();
  }
}

MutableArrayRef<InstOperand> Statement::getInstOperands() {
  switch (getKind()) {
  case Kind::OperationInst:
    return cast<OperationInst>(this)->getInstOperands();
  case Kind::For:
    return cast<ForStmt>(this)->getInstOperands();
  case Kind::If:
    return cast<IfStmt>(this)->getInstOperands();
  }
}

/// Emit a note about this statement, reporting up to any diagnostic
/// handlers that may be listening.
void Statement::emitNote(const Twine &message) const {
  getContext()->emitDiagnostic(getLoc(), message,
                               MLIRContext::DiagnosticKind::Note);
}

/// Emit a warning about this statement, reporting up to any diagnostic
/// handlers that may be listening.
void Statement::emitWarning(const Twine &message) const {
  getContext()->emitDiagnostic(getLoc(), message,
                               MLIRContext::DiagnosticKind::Warning);
}

/// Emit an error about fatal conditions with this operation, reporting up to
/// any diagnostic handlers that may be listening.  This function always
/// returns true.  NOTE: This may terminate the containing application, only
/// use when the IR is in an inconsistent state.
bool Statement::emitError(const Twine &message) const {
  return getContext()->emitError(getLoc(), message);
}

// Returns whether the Statement is a terminator.
bool Statement::isTerminator() const {
  if (auto *op = dyn_cast<OperationInst>(this))
    return op->isTerminator();
  return false;
}

//===----------------------------------------------------------------------===//
// ilist_traits for Statement
//===----------------------------------------------------------------------===//

void llvm::ilist_traits<::mlir::Statement>::deleteNode(Statement *stmt) {
  stmt->destroy();
}

StmtBlock *llvm::ilist_traits<::mlir::Statement>::getContainingBlock() {
  size_t Offset(
      size_t(&((StmtBlock *)nullptr->*StmtBlock::getSublistAccess(nullptr))));
  iplist<Statement> *Anchor(static_cast<iplist<Statement> *>(this));
  return reinterpret_cast<StmtBlock *>(reinterpret_cast<char *>(Anchor) -
                                       Offset);
}

/// This is a trait method invoked when a statement is added to a block.  We
/// keep the block pointer up to date.
void llvm::ilist_traits<::mlir::Statement>::addNodeToList(Statement *stmt) {
  assert(!stmt->getBlock() && "already in a statement block!");
  stmt->block = getContainingBlock();
}

/// This is a trait method invoked when a statement is removed from a block.
/// We keep the block pointer up to date.
void llvm::ilist_traits<::mlir::Statement>::removeNodeFromList(
    Statement *stmt) {
  assert(stmt->block && "not already in a statement block!");
  stmt->block = nullptr;
}

/// This is a trait method invoked when a statement is moved from one block
/// to another.  We keep the block pointer up to date.
void llvm::ilist_traits<::mlir::Statement>::transferNodesFromList(
    ilist_traits<Statement> &otherList, stmt_iterator first,
    stmt_iterator last) {
  // If we are transferring statements within the same block, the block
  // pointer doesn't need to be updated.
  StmtBlock *curParent = getContainingBlock();
  if (curParent == otherList.getContainingBlock())
    return;

  // Update the 'block' member of each statement.
  for (; first != last; ++first)
    first->block = curParent;
}

/// Remove this statement (and its descendants) from its StmtBlock and delete
/// all of them.
void Statement::erase() {
  assert(getBlock() && "Statement has no block");
  getBlock()->getStatements().erase(this);
}

/// Unlink this statement from its current block and insert it right before
/// `existingStmt` which may be in the same or another block in the same
/// function.
void Statement::moveBefore(Statement *existingStmt) {
  moveBefore(existingStmt->getBlock(), existingStmt->getIterator());
}

/// Unlink this operation instruction from its current basic block and insert
/// it right before `iterator` in the specified basic block.
void Statement::moveBefore(StmtBlock *block,
                           llvm::iplist<Statement>::iterator iterator) {
  block->getStatements().splice(iterator, getBlock()->getStatements(),
                                getIterator());
}

/// This drops all operand uses from this instruction, which is an essential
/// step in breaking cyclic dependences between references when they are to
/// be deleted.
void Statement::dropAllReferences() {
  for (auto &op : getInstOperands())
    op.drop();

  if (isTerminator())
    for (auto &dest : cast<OperationInst>(this)->getBlockOperands())
      dest.drop();
}

//===----------------------------------------------------------------------===//
// OperationInst
//===----------------------------------------------------------------------===//

/// Create a new OperationInst with the specific fields.
OperationInst *OperationInst::create(Location location, OperationName name,
                                     ArrayRef<Value *> operands,
                                     ArrayRef<Type> resultTypes,
                                     ArrayRef<NamedAttribute> attributes,
                                     ArrayRef<StmtBlock *> successors,
                                     MLIRContext *context) {
  unsigned numSuccessors = successors.size();

  // Input operands are nullptr-separated for each successors in the case of
  // terminators, the nullptr aren't actually stored.
  unsigned numOperands = operands.size() - numSuccessors;

  auto byteSize =
      totalSizeToAlloc<InstResult, StmtBlockOperand, unsigned, InstOperand>(
          resultTypes.size(), numSuccessors, numSuccessors, numOperands);
  void *rawMem = malloc(byteSize);

  // Initialize the OperationInst part of the statement.
  auto stmt = ::new (rawMem)
      OperationInst(location, name, numOperands, resultTypes.size(),
                    numSuccessors, attributes, context);

  // Initialize the results and operands.
  auto instResults = stmt->getInstResults();
  for (unsigned i = 0, e = resultTypes.size(); i != e; ++i)
    new (&instResults[i]) InstResult(resultTypes[i], stmt);

  auto InstOperands = stmt->getInstOperands();

  // Initialize normal operands.
  unsigned operandIt = 0, operandE = operands.size();
  unsigned nextOperand = 0;
  for (; operandIt != operandE; ++operandIt) {
    // Null operands are used as sentinals between successor operand lists. If
    // we encounter one here, break and handle the successor operands lists
    // separately below.
    if (!operands[operandIt])
      break;
    new (&InstOperands[nextOperand++]) InstOperand(stmt, operands[operandIt]);
  }

  unsigned currentSuccNum = 0;
  if (operandIt == operandE) {
    // Verify that the amount of sentinal operands is equivalent to the number
    // of successors.
    assert(currentSuccNum == numSuccessors);
    return stmt;
  }

  assert(stmt->isTerminator() &&
         "Sentinal operand found in non terminator operand list.");
  auto instBlockOperands = stmt->getBlockOperands();
  unsigned *succOperandCountIt = stmt->getTrailingObjects<unsigned>();
  unsigned *succOperandCountE = succOperandCountIt + numSuccessors;
  (void)succOperandCountE;

  for (; operandIt != operandE; ++operandIt) {
    // If we encounter a sentinal branch to the next operand update the count
    // variable.
    if (!operands[operandIt]) {
      assert(currentSuccNum < numSuccessors);

      // After the first iteration update the successor operand count
      // variable.
      if (currentSuccNum != 0) {
        ++succOperandCountIt;
        assert(succOperandCountIt != succOperandCountE &&
               "More sentinal operands than successors.");
      }

      new (&instBlockOperands[currentSuccNum])
          StmtBlockOperand(stmt, successors[currentSuccNum]);
      *succOperandCountIt = 0;
      ++currentSuccNum;
      continue;
    }
    new (&InstOperands[nextOperand++]) InstOperand(stmt, operands[operandIt]);
    ++(*succOperandCountIt);
  }

  // Verify that the amount of sentinal operands is equivalent to the number of
  // successors.
  assert(currentSuccNum == numSuccessors);

  return stmt;
}

OperationInst::OperationInst(Location location, OperationName name,
                             unsigned numOperands, unsigned numResults,
                             unsigned numSuccessors,
                             ArrayRef<NamedAttribute> attributes,
                             MLIRContext *context)
    : Statement(Kind::OperationInst, location), numOperands(numOperands),
      numResults(numResults), numSuccs(numSuccessors), name(name) {
#ifndef NDEBUG
  for (auto elt : attributes)
    assert(elt.second != nullptr && "Attributes cannot have null entries");
#endif

  this->attrs = AttributeListStorage::get(attributes, context);
}

OperationInst::~OperationInst() {
  // Explicitly run the destructors for the operands and results.
  for (auto &operand : getInstOperands())
    operand.~InstOperand();

  for (auto &result : getInstResults())
    result.~InstResult();

  // Explicitly run the destructors for the successors.
  if (isTerminator())
    for (auto &successor : getBlockOperands())
      successor.~StmtBlockOperand();
}

/// Return true if there are no users of any results of this operation.
bool OperationInst::use_empty() const {
  for (auto *result : getResults())
    if (!result->use_empty())
      return false;
  return true;
}

ArrayRef<NamedAttribute> OperationInst::getAttrs() const {
  if (!attrs)
    return {};
  return attrs->getElements();
}

void OperationInst::destroy() {
  this->~OperationInst();
  free(this);
}

/// Return the context this operation is associated with.
MLIRContext *OperationInst::getContext() const {
  // If we have a result or operand type, that is a constant time way to get
  // to the context.
  if (getNumResults())
    return getResult(0)->getType().getContext();
  if (getNumOperands())
    return getOperand(0)->getType().getContext();

  // In the very odd case where we have no operands or results, fall back to
  // doing a find.
  return getFunction()->getContext();
}

bool OperationInst::isReturn() const { return isa<ReturnOp>(); }

void OperationInst::setSuccessor(BasicBlock *block, unsigned index) {
  assert(index < getNumSuccessors());
  getBlockOperands()[index].set(block);
}

void OperationInst::eraseOperand(unsigned index) {
  assert(index < getNumOperands());
  auto Operands = getInstOperands();
  // Shift all operands down by 1.
  std::rotate(&Operands[index], &Operands[index + 1],
              &Operands[numOperands - 1]);
  --numOperands;
  Operands[getNumOperands()].~InstOperand();
}

auto OperationInst::getSuccessorOperands(unsigned index) const
    -> llvm::iterator_range<const_operand_iterator> {
  assert(isTerminator() && "Only terminators have successors.");
  unsigned succOperandIndex = getSuccessorOperandIndex(index);
  return {const_operand_iterator(this, succOperandIndex),
          const_operand_iterator(this, succOperandIndex +
                                           getNumSuccessorOperands(index))};
}
auto OperationInst::getSuccessorOperands(unsigned index)
    -> llvm::iterator_range<operand_iterator> {
  assert(isTerminator() && "Only terminators have successors.");
  unsigned succOperandIndex = getSuccessorOperandIndex(index);
  return {operand_iterator(this, succOperandIndex),
          operand_iterator(this,
                           succOperandIndex + getNumSuccessorOperands(index))};
}

/// If an attribute exists with the specified name, change it to the new
/// value.  Otherwise, add a new attribute with the specified name/value.
void OperationInst::setAttr(Identifier name, Attribute value) {
  assert(value && "attributes may never be null");
  auto origAttrs = getAttrs();

  SmallVector<NamedAttribute, 8> newAttrs(origAttrs.begin(), origAttrs.end());
  auto *context = getContext();

  // If we already have this attribute, replace it.
  for (auto &elt : newAttrs)
    if (elt.first == name) {
      elt.second = value;
      attrs = AttributeListStorage::get(newAttrs, context);
      return;
    }

  // Otherwise, add it.
  newAttrs.push_back({name, value});
  attrs = AttributeListStorage::get(newAttrs, context);
}

/// Remove the attribute with the specified name if it exists.  The return
/// value indicates whether the attribute was present or not.
auto OperationInst::removeAttr(Identifier name) -> RemoveResult {
  auto origAttrs = getAttrs();
  for (unsigned i = 0, e = origAttrs.size(); i != e; ++i) {
    if (origAttrs[i].first == name) {
      SmallVector<NamedAttribute, 8> newAttrs;
      newAttrs.reserve(origAttrs.size() - 1);
      newAttrs.append(origAttrs.begin(), origAttrs.begin() + i);
      newAttrs.append(origAttrs.begin() + i + 1, origAttrs.end());
      attrs = AttributeListStorage::get(newAttrs, getContext());
      return RemoveResult::Removed;
    }
  }
  return RemoveResult::NotFound;
}

/// Attempt to constant fold this operation with the specified constant
/// operand values.  If successful, this returns false and fills in the
/// results vector.  If not, this returns true and results is unspecified.
bool OperationInst::constantFold(ArrayRef<Attribute> operands,
                                 SmallVectorImpl<Attribute> &results) const {
  if (auto *abstractOp = getAbstractOperation()) {
    // If we have a registered operation definition matching this one, use it to
    // try to constant fold the operation.
    if (!abstractOp->constantFoldHook(llvm::cast<OperationInst>(this), operands,
                                      results))
      return false;

    // Otherwise, fall back on the dialect hook to handle it.
    return abstractOp->dialect.constantFoldHook(llvm::cast<OperationInst>(this),
                                                operands, results);
  }

  // If this operation hasn't been registered or doesn't have abstract
  // operation, fall back to a dialect which matches the prefix.
  auto opName = getName().getStringRef();
  if (auto *dialect = getContext()->getRegisteredDialect(opName)) {
    return dialect->constantFoldHook(llvm::cast<OperationInst>(this), operands,
                                     results);
  }

  return true;
}

/// Emit an error with the op name prefixed, like "'dim' op " which is
/// convenient for verifiers.
bool OperationInst::emitOpError(const Twine &message) const {
  return emitError(Twine('\'') + getName().getStringRef() + "' op " + message);
}

//===----------------------------------------------------------------------===//
// ForStmt
//===----------------------------------------------------------------------===//

ForStmt *ForStmt::create(Location location, ArrayRef<Value *> lbOperands,
                         AffineMap lbMap, ArrayRef<Value *> ubOperands,
                         AffineMap ubMap, int64_t step) {
  assert(lbOperands.size() == lbMap.getNumInputs() &&
         "lower bound operand count does not match the affine map");
  assert(ubOperands.size() == ubMap.getNumInputs() &&
         "upper bound operand count does not match the affine map");
  assert(step > 0 && "step has to be a positive integer constant");

  unsigned numOperands = lbOperands.size() + ubOperands.size();
  ForStmt *stmt = new ForStmt(location, numOperands, lbMap, ubMap, step);

  unsigned i = 0;
  for (unsigned e = lbOperands.size(); i != e; ++i)
    stmt->operands.emplace_back(InstOperand(stmt, lbOperands[i]));

  for (unsigned j = 0, e = ubOperands.size(); j != e; ++i, ++j)
    stmt->operands.emplace_back(InstOperand(stmt, ubOperands[j]));

  return stmt;
}

ForStmt::ForStmt(Location location, unsigned numOperands, AffineMap lbMap,
                 AffineMap ubMap, int64_t step)
    : Statement(Statement::Kind::For, location),
      Value(Value::Kind::ForStmt,
            Type::getIndex(lbMap.getResult(0).getContext())),
      body(this), lbMap(lbMap), ubMap(ubMap), step(step) {

  // The body of a for stmt always has one block.
  body.push_back(new StmtBlock());
  operands.reserve(numOperands);
}

const AffineBound ForStmt::getLowerBound() const {
  return AffineBound(*this, 0, lbMap.getNumInputs(), lbMap);
}

const AffineBound ForStmt::getUpperBound() const {
  return AffineBound(*this, lbMap.getNumInputs(), getNumOperands(), ubMap);
}

void ForStmt::setLowerBound(ArrayRef<Value *> lbOperands, AffineMap map) {
  assert(lbOperands.size() == map.getNumInputs());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");

  SmallVector<Value *, 4> ubOperands(getUpperBoundOperands());

  operands.clear();
  operands.reserve(lbOperands.size() + ubMap.getNumInputs());
  for (auto *operand : lbOperands) {
    operands.emplace_back(InstOperand(this, operand));
  }
  for (auto *operand : ubOperands) {
    operands.emplace_back(InstOperand(this, operand));
  }
  this->lbMap = map;
}

void ForStmt::setUpperBound(ArrayRef<Value *> ubOperands, AffineMap map) {
  assert(ubOperands.size() == map.getNumInputs());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");

  SmallVector<Value *, 4> lbOperands(getLowerBoundOperands());

  operands.clear();
  operands.reserve(lbOperands.size() + ubOperands.size());
  for (auto *operand : lbOperands) {
    operands.emplace_back(InstOperand(this, operand));
  }
  for (auto *operand : ubOperands) {
    operands.emplace_back(InstOperand(this, operand));
  }
  this->ubMap = map;
}

void ForStmt::setLowerBoundMap(AffineMap map) {
  assert(lbMap.getNumDims() == map.getNumDims() &&
         lbMap.getNumSymbols() == map.getNumSymbols());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");
  this->lbMap = map;
}

void ForStmt::setUpperBoundMap(AffineMap map) {
  assert(ubMap.getNumDims() == map.getNumDims() &&
         ubMap.getNumSymbols() == map.getNumSymbols());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");
  this->ubMap = map;
}

bool ForStmt::hasConstantLowerBound() const { return lbMap.isSingleConstant(); }

bool ForStmt::hasConstantUpperBound() const { return ubMap.isSingleConstant(); }

int64_t ForStmt::getConstantLowerBound() const {
  return lbMap.getSingleConstantResult();
}

int64_t ForStmt::getConstantUpperBound() const {
  return ubMap.getSingleConstantResult();
}

void ForStmt::setConstantLowerBound(int64_t value) {
  setLowerBound({}, AffineMap::getConstantMap(value, getContext()));
}

void ForStmt::setConstantUpperBound(int64_t value) {
  setUpperBound({}, AffineMap::getConstantMap(value, getContext()));
}

ForStmt::operand_range ForStmt::getLowerBoundOperands() {
  return {operand_begin(), operand_begin() + getLowerBoundMap().getNumInputs()};
}

ForStmt::const_operand_range ForStmt::getLowerBoundOperands() const {
  return {operand_begin(), operand_begin() + getLowerBoundMap().getNumInputs()};
}

ForStmt::operand_range ForStmt::getUpperBoundOperands() {
  return {operand_begin() + getLowerBoundMap().getNumInputs(), operand_end()};
}

ForStmt::const_operand_range ForStmt::getUpperBoundOperands() const {
  return {operand_begin() + getLowerBoundMap().getNumInputs(), operand_end()};
}

bool ForStmt::matchingBoundOperandList() const {
  if (lbMap.getNumDims() != ubMap.getNumDims() ||
      lbMap.getNumSymbols() != ubMap.getNumSymbols())
    return false;

  unsigned numOperands = lbMap.getNumInputs();
  for (unsigned i = 0, e = lbMap.getNumInputs(); i < e; i++) {
    // Compare Value *'s.
    if (getOperand(i) != getOperand(numOperands + i))
      return false;
  }
  return true;
}

//===----------------------------------------------------------------------===//
// IfStmt
//===----------------------------------------------------------------------===//

IfStmt::IfStmt(Location location, unsigned numOperands, IntegerSet set)
    : Statement(Kind::If, location), thenClause(this), elseClause(nullptr),
      set(set) {
  operands.reserve(numOperands);

  // The then of an 'if' stmt always has one block.
  thenClause.push_back(new StmtBlock());
}

IfStmt::~IfStmt() {
  if (elseClause)
    delete elseClause;

  // An IfStmt's IntegerSet 'set' should not be deleted since it is
  // allocated through MLIRContext's bump pointer allocator.
}

IfStmt *IfStmt::create(Location location, ArrayRef<Value *> operands,
                       IntegerSet set) {
  unsigned numOperands = operands.size();
  assert(numOperands == set.getNumOperands() &&
         "operand cound does not match the integer set operand count");

  IfStmt *stmt = new IfStmt(location, numOperands, set);

  for (auto *op : operands)
    stmt->operands.emplace_back(InstOperand(stmt, op));

  return stmt;
}

const AffineCondition IfStmt::getCondition() const {
  return AffineCondition(*this, set);
}

MLIRContext *IfStmt::getContext() const {
  // Check for degenerate case of if statement with no operands.
  // This is unlikely, but legal.
  if (operands.empty())
    return getFunction()->getContext();

  return getOperand(0)->getType().getContext();
}

//===----------------------------------------------------------------------===//
// Statement Cloning
//===----------------------------------------------------------------------===//

/// Create a deep copy of this statement, remapping any operands that use
/// values outside of the statement using the map that is provided (leaving
/// them alone if no entry is present).  Replaces references to cloned
/// sub-statements to the corresponding statement that is copied, and adds
/// those mappings to the map.
Statement *Statement::clone(DenseMap<const Value *, Value *> &operandMap,
                            MLIRContext *context) const {
  // If the specified value is in operandMap, return the remapped value.
  // Otherwise return the value itself.
  auto remapOperand = [&](const Value *value) -> Value * {
    auto it = operandMap.find(value);
    return it != operandMap.end() ? it->second : const_cast<Value *>(value);
  };

  SmallVector<Value *, 8> operands;
  SmallVector<StmtBlock *, 2> successors;
  if (auto *opStmt = dyn_cast<OperationInst>(this)) {
    operands.reserve(getNumOperands() + opStmt->getNumSuccessors());

    if (!opStmt->isTerminator()) {
      // Non-terminators just add all the operands.
      for (auto *opValue : getOperands())
        operands.push_back(remapOperand(opValue));
    } else {
      // We add the operands separated by nullptr's for each successor.
      unsigned firstSuccOperand = opStmt->getNumSuccessors()
                                      ? opStmt->getSuccessorOperandIndex(0)
                                      : opStmt->getNumOperands();
      auto InstOperands = opStmt->getInstOperands();

      unsigned i = 0;
      for (; i != firstSuccOperand; ++i)
        operands.push_back(remapOperand(InstOperands[i].get()));

      successors.reserve(opStmt->getNumSuccessors());
      for (unsigned succ = 0, e = opStmt->getNumSuccessors(); succ != e;
           ++succ) {
        successors.push_back(
            const_cast<StmtBlock *>(opStmt->getSuccessor(succ)));

        // Add sentinel to delineate successor operands.
        operands.push_back(nullptr);

        // Remap the successors operands.
        for (auto *operand : opStmt->getSuccessorOperands(succ))
          operands.push_back(remapOperand(operand));
      }
    }

    SmallVector<Type, 8> resultTypes;
    resultTypes.reserve(opStmt->getNumResults());
    for (auto *result : opStmt->getResults())
      resultTypes.push_back(result->getType());
    auto *newOp = OperationInst::create(getLoc(), opStmt->getName(), operands,
                                        resultTypes, opStmt->getAttrs(),
                                        successors, context);
    // Remember the mapping of any results.
    for (unsigned i = 0, e = opStmt->getNumResults(); i != e; ++i)
      operandMap[opStmt->getResult(i)] = newOp->getResult(i);
    return newOp;
  }

  operands.reserve(getNumOperands());
  for (auto *opValue : getOperands())
    operands.push_back(remapOperand(opValue));

  if (auto *forStmt = dyn_cast<ForStmt>(this)) {
    auto lbMap = forStmt->getLowerBoundMap();
    auto ubMap = forStmt->getUpperBoundMap();

    auto *newFor = ForStmt::create(
        getLoc(), ArrayRef<Value *>(operands).take_front(lbMap.getNumInputs()),
        lbMap, ArrayRef<Value *>(operands).take_back(ubMap.getNumInputs()),
        ubMap, forStmt->getStep());

    // Remember the induction variable mapping.
    operandMap[forStmt] = newFor;

    // Recursively clone the body of the for loop.
    for (auto &subStmt : *forStmt->getBody())
      newFor->getBody()->push_back(subStmt.clone(operandMap, context));

    return newFor;
  }

  // Otherwise, we must have an If statement.
  auto *ifStmt = cast<IfStmt>(this);
  auto *newIf = IfStmt::create(getLoc(), operands, ifStmt->getIntegerSet());

  auto *resultThen = newIf->getThen();
  for (auto &childStmt : *ifStmt->getThen())
    resultThen->push_back(childStmt.clone(operandMap, context));

  if (ifStmt->hasElse()) {
    auto *resultElse = newIf->createElse();
    for (auto &childStmt : *ifStmt->getElse())
      resultElse->push_back(childStmt.clone(operandMap, context));
  }

  return newIf;
}

Statement *Statement::clone(MLIRContext *context) const {
  DenseMap<const Value *, Value *> operandMap;
  return clone(operandMap, context);
}
