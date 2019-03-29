//===- Verifier.cpp - MLIR Verifier Implementation ------------------------===//
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
// This file implements the verify() methods on the various IR types, performing
// (potentially expensive) checks on the holistic structure of the code.  This
// can be used for detecting bugs in compiler transformations and hand written
// .mlir files.
//
// The checks in this file are only for things that can occur as part of IR
// transformations: e.g. violation of dominance information, malformed operation
// attributes, etc.  MLIR supports transformations moving IR through locally
// invalid states (e.g. unlinking an instruction from an instruction before
// re-inserting it in a new place), but each transformation must complete with
// the IR in a valid form.
//
// This should not check for things that are always wrong by construction (e.g.
// affine maps or other immutable structures that are incorrect), because those
// are not mutable and can be checked at time of construction.
//
//===----------------------------------------------------------------------===//

#include "mlir/Analysis/Dominance.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/CFGFunction.h"
#include "mlir/IR/MLFunction.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/Statements.h"
#include "mlir/IR/StmtVisitor.h"
#include "llvm/ADT/ScopedHashTable.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/raw_ostream.h"
using namespace mlir;

namespace {
/// Base class for the verifiers in this file.  It is a pervasive truth that
/// this file treats "true" as an error that needs to be recovered from, and
/// "false" as success.
///
class Verifier {
public:
  bool failure(const Twine &message, const Operation &value) {
    value.emitError(message);
    return true;
  }

  bool failure(const Twine &message, const Function &fn) {
    fn.emitError(message);
    return true;
  }

  bool failure(const Twine &message, const Instruction &inst) {
    inst.emitError(message);
    return true;
  }

  bool failure(const Twine &message, const BasicBlock &bb) {
    // Take the location information for the first instruction in the block.
    if (!bb.empty())
      return failure(message, static_cast<const Instruction &>(bb.front()));

    // If the code is properly formed, there will be a terminator.  Use its
    // location.
    if (auto *termInst = bb.getTerminator())
      return failure(message, *termInst);

    // Worst case, fall back to using the function's location.
    return failure(message, fn);
  }

  bool verifyOperation(const Operation &op);
  bool verifyAttribute(Attribute *attr, const Operation &op);

protected:
  explicit Verifier(const Function &fn) : fn(fn) {}

private:
  /// The function being checked.
  const Function &fn;
};
} // end anonymous namespace

// Check that function attributes are all well formed.
bool Verifier::verifyAttribute(Attribute *attr, const Operation &op) {
  if (!attr->isOrContainsFunction())
    return false;

  // If we have a function attribute, check that it is non-null and in the
  // same module as the operation that refers to it.
  if (auto *fnAttr = dyn_cast<FunctionAttr>(attr)) {
    if (!fnAttr->getValue())
      return failure("attribute refers to deallocated function!", op);

    if (fnAttr->getValue()->getModule() != fn.getModule())
      return failure("attribute refers to function '" +
                         Twine(fnAttr->getValue()->getName()) +
                         "' defined in another module!",
                     op);
    return false;
  }

  // Otherwise, we must have an array attribute, remap the elements.
  for (auto *elt : cast<ArrayAttr>(attr)->getValue()) {
    if (verifyAttribute(elt, op))
      return true;
  }

  return false;
}

/// Check the invariants of the specified operation instruction or statement.
bool Verifier::verifyOperation(const Operation &op) {
  if (op.getOperationFunction() != &fn)
    return failure("operation in the wrong function", op);

  // Check that operands are non-nil and structurally ok.
  for (const auto *operand : op.getOperands()) {
    if (!operand)
      return failure("null operand found", op);

    if (operand->getFunction() != &fn)
      return failure("reference to operand defined in another function", op);
  }

  // Verify all attributes are ok.  We need to check Function attributes, since
  // they are actually mutable (the function they refer to can be deleted), and
  // we have to check array attributes that can refer to them.
  for (auto attr : op.getAttrs()) {
    if (verifyAttribute(attr.second, op))
      return true;
  }

  // If we can get operation info for this, check the custom hook.
  if (auto *opInfo = op.getAbstractOperation()) {
    if (opInfo->verifyInvariants(&op))
      return true;
  }

  return false;
}

//===----------------------------------------------------------------------===//
// CFG Functions
//===----------------------------------------------------------------------===//

namespace {
struct CFGFuncVerifier : public Verifier {
  const CFGFunction &fn;
  DominanceInfo domInfo;

  CFGFuncVerifier(const CFGFunction &fn)
      : Verifier(fn), fn(fn), domInfo(const_cast<CFGFunction *>(&fn)) {}

  bool verify();
  bool verifyBlock(const BasicBlock &block);
  bool verifyTerminator(const TerminatorInst &term);
  bool verifyInstOperands(const Instruction &inst);

  bool verifyBBArguments(ArrayRef<InstOperand> operands,
                         const BasicBlock *destBB, const TerminatorInst &term);
  bool verifyReturn(const ReturnInst &inst);
  bool verifyBranch(const BranchInst &inst);
  bool verifyCondBranch(const CondBranchInst &inst);
};
} // end anonymous namespace

bool CFGFuncVerifier::verify() {
  llvm::PrettyStackTraceFormat fmt("MLIR Verifier: cfgfunc @%s",
                                   fn.getName().c_str());

  // TODO: Lots to be done here, including verifying dominance information when
  // we have uses and defs.
  // TODO: Verify the first block has no predecessors.

  if (fn.empty())
    return failure("cfgfunc must have at least one basic block", fn);

  // Verify that the argument list of the function and the arg list of the first
  // block line up.
  auto *firstBB = &fn.front();
  auto fnInputTypes = fn.getType()->getInputs();
  if (fnInputTypes.size() != firstBB->getNumArguments())
    return failure("first block of cfgfunc must have " +
                       Twine(fnInputTypes.size()) +
                       " arguments to match function signature",
                   fn);
  for (unsigned i = 0, e = firstBB->getNumArguments(); i != e; ++i)
    if (fnInputTypes[i] != firstBB->getArgument(i)->getType())
      return failure(
          "type of argument #" + Twine(i) +
              " must match corresponding argument in function signature",
          fn);

  for (auto &block : fn) {
    if (verifyBlock(block))
      return true;
  }
  return false;
}

bool CFGFuncVerifier::verifyInstOperands(const Instruction &inst) {
  // Check that operands properly dominate this use.
  for (unsigned operandNo = 0, e = inst.getNumOperands(); operandNo != e;
       ++operandNo) {
    auto *op = inst.getOperand(operandNo);
    if (domInfo.properlyDominates(op, &inst))
      continue;

    inst.emitError("operand #" + Twine(operandNo) +
                   " does not dominate this use");
    if (auto *useInst = op->getDefiningInst())
      useInst->emitNote("operand defined here");
    return true;
  }

  return false;
}

bool CFGFuncVerifier::verifyBlock(const BasicBlock &block) {
  if (!block.getTerminator())
    return failure("basic block with no terminator", block);

  if (verifyTerminator(*block.getTerminator()))
    return true;

  for (auto *arg : block.getArguments()) {
    if (arg->getOwner() != &block)
      return failure("basic block argument not owned by block", block);
  }

  for (auto &inst : block) {
    if (verifyOperation(inst) || verifyInstOperands(inst))
      return true;
  }
  return false;
}

bool CFGFuncVerifier::verifyTerminator(const TerminatorInst &term) {
  if (term.getFunction() != &fn)
    return failure("terminator in the wrong function", term);

  // Check that operands are non-nil and structurally ok.
  for (const auto *operand : term.getOperands()) {
    if (!operand)
      return failure("null operand found", term);

    if (operand->getFunction() != &fn)
      return failure("reference to operand defined in another function", term);
  }

  // Verify dominance of values.
  verifyInstOperands(term);

  // Check that successors are in the right function.
  for (auto *succ : term.getBlock()->getSuccessors()) {
    if (succ->getFunction() != &fn)
      return failure("reference to block defined in another function", term);
  }

  if (auto *ret = dyn_cast<ReturnInst>(&term))
    return verifyReturn(*ret);

  if (auto *br = dyn_cast<BranchInst>(&term))
    return verifyBranch(*br);

  if (auto *br = dyn_cast<CondBranchInst>(&term))
    return verifyCondBranch(*br);

  return false;
}

/// Check a set of basic block arguments against the expected list in in the
/// destination basic block.
bool CFGFuncVerifier::verifyBBArguments(ArrayRef<InstOperand> operands,
                                        const BasicBlock *destBB,
                                        const TerminatorInst &term) {
  if (operands.size() != destBB->getNumArguments())
    return failure("branch has " + Twine(operands.size()) +
                       " operands, but target block has " +
                       Twine(destBB->getNumArguments()),
                   term);

  for (unsigned i = 0, e = operands.size(); i != e; ++i)
    if (operands[i].get()->getType() != destBB->getArgument(i)->getType())
      return failure("type mismatch in bb argument #" + Twine(i), term);

  return false;
}

bool CFGFuncVerifier::verifyReturn(const ReturnInst &inst) {
  // Verify that the return operands match the results of the function.
  auto results = fn.getType()->getResults();
  if (inst.getNumOperands() != results.size())
    return failure("return has " + Twine(inst.getNumOperands()) +
                       " operands, but enclosing function returns " +
                       Twine(results.size()),
                   inst);

  for (unsigned i = 0, e = results.size(); i != e; ++i)
    if (inst.getOperand(i)->getType() != results[i])
      return failure("type of return operand " + Twine(i) +
                         " doesn't match function result type",
                     inst);

  return false;
}

bool CFGFuncVerifier::verifyBranch(const BranchInst &inst) {
  // Verify that the number of operands lines up with the number of BB arguments
  // in the successor.
  if (verifyBBArguments(inst.getInstOperands(), inst.getDest(), inst))
    return true;

  return false;
}

bool CFGFuncVerifier::verifyCondBranch(const CondBranchInst &inst) {
  // Verify that the number of operands lines up with the number of BB arguments
  // in the true successor.
  if (verifyBBArguments(inst.getTrueInstOperands(), inst.getTrueDest(), inst))
    return true;

  // And the false successor.
  if (verifyBBArguments(inst.getFalseInstOperands(), inst.getFalseDest(), inst))
    return true;

  if (inst.getCondition()->getType() != Type::getInteger(1, fn.getContext()))
    return failure("type of condition is not boolean (i1)", inst);

  return false;
}

//===----------------------------------------------------------------------===//
// ML Functions
//===----------------------------------------------------------------------===//

namespace {
struct MLFuncVerifier : public Verifier, public StmtWalker<MLFuncVerifier> {
  const MLFunction &fn;
  bool hadError = false;

  MLFuncVerifier(const MLFunction &fn) : Verifier(fn), fn(fn) {}

  void visitOperationStmt(OperationStmt *opStmt) {
    hadError |= verifyOperation(*opStmt);
  }

  bool verify() {
    llvm::PrettyStackTraceFormat fmt("MLIR Verifier: mlfunc @%s",
                                     fn.getName().c_str());

    // Check basic structural properties.
    walk(const_cast<MLFunction *>(&fn));
    if (hadError)
      return true;

    // TODO: check that operation is not a return statement unless it's
    // the last one in the function.
    // TODO: check that loop bounds and if conditions are properly formed.
    if (verifyReturn())
      return true;

    return verifyDominance();
  }

  /// Walk all of the code in this MLFunc and verify that the operands of any
  /// operations are properly dominated by their definitions.
  bool verifyDominance();

  /// Verify that function has a return statement that matches its signature.
  bool verifyReturn();
};
} // end anonymous namespace

/// Walk all of the code in this MLFunc and verify that the operands of any
/// operations are properly dominated by their definitions.
bool MLFuncVerifier::verifyDominance() {
  using HashTable = llvm::ScopedHashTable<const SSAValue *, bool>;
  HashTable liveValues;
  HashTable::ScopeTy topScope(liveValues);

  // All of the arguments to the function are live for the whole function.
  for (auto *arg : fn.getArguments())
    liveValues.insert(arg, true);

  // This recursive function walks the statement list pushing scopes onto the
  // stack as it goes, and popping them to remove them from the table.
  std::function<bool(const StmtBlock &block)> walkBlock;
  walkBlock = [&](const StmtBlock &block) -> bool {
    HashTable::ScopeTy blockScope(liveValues);

    // The induction variable of a for statement is live within its body.
    if (auto *forStmt = dyn_cast<ForStmt>(&block))
      liveValues.insert(forStmt, true);

    for (auto &stmt : block) {
      // Verify that each of the operands are live.
      unsigned operandNo = 0;
      for (auto *opValue : stmt.getOperands()) {
        if (!liveValues.count(opValue)) {
          stmt.emitError("operand #" + Twine(operandNo) +
                         " does not dominate this use");
          if (auto *useStmt = opValue->getDefiningStmt())
            useStmt->emitNote("operand defined here");
          return true;
        }
        ++operandNo;
      }

      if (auto *opStmt = dyn_cast<OperationStmt>(&stmt)) {
        // Operations define values, add them to the hash table.
        for (auto *result : opStmt->getResults())
          liveValues.insert(result, true);
        continue;
      }

      // If this is an if or for, recursively walk the block they contain.
      if (auto *ifStmt = dyn_cast<IfStmt>(&stmt)) {
        if (walkBlock(*ifStmt->getThen()))
          return true;

        if (auto *elseClause = ifStmt->getElse())
          if (walkBlock(*elseClause))
            return true;
      }
      if (auto *forStmt = dyn_cast<ForStmt>(&stmt))
        if (walkBlock(*forStmt))
          return true;
    }

    return false;
  };

  // Check the whole function out.
  return walkBlock(fn);
}

bool MLFuncVerifier::verifyReturn() {
  // TODO: fold return verification in the pass that verifies all statements.
  const char missingReturnMsg[] = "ML function must end with return statement";
  if (fn.getStatements().empty())
    return failure(missingReturnMsg, fn);

  const auto &stmt = fn.getStatements().back();
  if (const auto *op = dyn_cast<OperationStmt>(&stmt)) {
    if (!op->isReturn())
      return failure(missingReturnMsg, fn);

    // The operand number and types must match the function signature.
    // TODO: move this verification in ReturnOp::verify() if printing
    // of the error messages below can be made to work there.
    const auto &results = fn.getType()->getResults();
    if (op->getNumOperands() != results.size())
      return failure("return has " + Twine(op->getNumOperands()) +
                         " operands, but enclosing function returns " +
                         Twine(results.size()),
                     *op);

    for (unsigned i = 0, e = results.size(); i != e; ++i)
      if (op->getOperand(i)->getType() != results[i])
        return failure("type of return operand " + Twine(i) +
                           " doesn't match function result type",
                       *op);
    return false;
  }
  return failure(missingReturnMsg, fn);
}

//===----------------------------------------------------------------------===//
// Entrypoints
//===----------------------------------------------------------------------===//

/// Perform (potentially expensive) checks of invariants, used to detect
/// compiler bugs.  On error, this reports the error through the MLIRContext and
/// returns true.
bool Function::verify() const {
  switch (getKind()) {
  case Kind::ExtFunc:
    // No body, nothing can be wrong here.
    return false;
  case Kind::CFGFunc:
    return CFGFuncVerifier(*cast<CFGFunction>(this)).verify();
  case Kind::MLFunc:
    return MLFuncVerifier(*cast<MLFunction>(this)).verify();
  }
}

/// Perform (potentially expensive) checks of invariants, used to detect
/// compiler bugs.  On error, this reports the error through the MLIRContext and
/// returns true.
bool Module::verify() const {

  /// Check that each function is correct.
  for (auto &fn : *this) {
    if (fn.verify())
      return true;
  }

  return false;
}
