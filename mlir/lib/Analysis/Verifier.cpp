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
// invalid states (e.g. unlinking an operation from a block before re-inserting
// it in a new place), but each transformation must complete with the IR in a
// valid form.
//
// This should not check for things that are always wrong by construction (e.g.
// affine maps or other immutable structures that are incorrect), because those
// are not mutable and can be checked at time of construction.
//
//===----------------------------------------------------------------------===//

#include "mlir/Analysis/Dominance.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/Operation.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"
using namespace mlir;

namespace {
/// This class encapsulates all the state used to verify a function body.  It is
/// a pervasive truth that this file treats "true" as an error that needs to be
/// recovered from, and "false" as success.
///
class FuncVerifier {
public:
  LogicalResult failure() { return mlir::failure(); }

  LogicalResult failure(const Twine &message, Operation &value) {
    return value.emitError(message);
  }

  LogicalResult failure(const Twine &message, Function &fn) {
    return fn.emitError(message);
  }

  LogicalResult failure(const Twine &message, Block &bb) {
    // Take the location information for the first operation in the block.
    if (!bb.empty())
      return failure(message, bb.front());

    // Worst case, fall back to using the function's location.
    return failure(message, fn);
  }

  /// Returns the registered dialect for a dialect-specific attribute.
  Dialect *getDialectForAttribute(const NamedAttribute &attr) {
    assert(attr.first.strref().contains('.') && "expected dialect attribute");
    auto dialectNamePair = attr.first.strref().split('.');
    return fn.getContext()->getRegisteredDialect(dialectNamePair.first);
  }

  LogicalResult verify();
  LogicalResult verifyBlock(Block &block, bool isTopLevel);
  LogicalResult verifyOperation(Operation &op);
  LogicalResult verifyDominance(Block &block);
  LogicalResult verifyOpDominance(Operation &op);

  explicit FuncVerifier(Function &fn)
      : fn(fn), identifierRegex("^[a-zA-Z_][a-zA-Z_0-9\\.\\$]*$") {}

private:
  /// The function being checked.
  Function &fn;

  /// Dominance information for this function, when checking dominance.
  DominanceInfo *domInfo = nullptr;

  /// Regex checker for attribute names.
  llvm::Regex identifierRegex;

  /// Mapping between dialect namespace and if that dialect supports
  /// unregistered operations.
  llvm::StringMap<bool> dialectAllowsUnknownOps;
};
} // end anonymous namespace

LogicalResult FuncVerifier::verify() {
  llvm::PrettyStackTraceFormat fmt("MLIR Verifier: func @%s",
                                   fn.getName().c_str());

  // Check that the function name is valid.
  if (!identifierRegex.match(fn.getName().strref()))
    return failure("invalid function name '" + fn.getName().strref() + "'", fn);

  /// Verify that all of the attributes are okay.
  for (auto attr : fn.getAttrs()) {
    if (!identifierRegex.match(attr.first))
      return failure("invalid attribute name '" + attr.first.strref() + "'",
                     fn);

    /// Check that the attribute is a dialect attribute, i.e. contains a '.' for
    /// the namespace.
    if (!attr.first.strref().contains('.'))
      return failure("functions may only have dialect attributes", fn);

    // Verify this attribute with the defining dialect.
    if (auto *dialect = getDialectForAttribute(attr))
      if (failed(dialect->verifyFunctionAttribute(&fn, attr)))
        return failure();
  }

  /// Verify that all of the argument attributes are okay.
  for (unsigned i = 0, e = fn.getNumArguments(); i != e; ++i) {
    for (auto attr : fn.getArgAttrs(i)) {
      if (!identifierRegex.match(attr.first))
        return failure(
            llvm::formatv("invalid attribute name '{0}' on argument {1}",
                          attr.first.strref(), i),
            fn);

      /// Check that the attribute is a dialect attribute, i.e. contains a '.'
      /// for the namespace.
      if (!attr.first.strref().contains('.'))
        return failure("function arguments may only have dialect attributes",
                       fn);

      // Verify this attribute with the defining dialect.
      if (auto *dialect = getDialectForAttribute(attr))
        if (failed(dialect->verifyFunctionArgAttribute(&fn, i, attr)))
          return failure();
    }
  }

  // External functions have nothing more to check.
  if (fn.isExternal())
    return success();

  // Verify the first block has no predecessors.
  auto *firstBB = &fn.front();
  if (!firstBB->hasNoPredecessors())
    return failure("entry block of function may not have predecessors", fn);

  // Verify that the argument list of the function and the arg list of the first
  // block line up.
  auto fnInputTypes = fn.getType().getInputs();
  if (fnInputTypes.size() != firstBB->getNumArguments())
    return failure("first block of function must have " +
                       Twine(fnInputTypes.size()) +
                       " arguments to match function signature",
                   fn);
  for (unsigned i = 0, e = firstBB->getNumArguments(); i != e; ++i)
    if (fnInputTypes[i] != firstBB->getArgument(i)->getType())
      return failure(
          "type of argument #" + Twine(i) +
              " must match corresponding argument in function signature",
          fn);

  for (auto &block : fn)
    if (failed(verifyBlock(block, /*isTopLevel=*/true)))
      return failure();

  // Since everything looks structurally ok to this point, we do a dominance
  // check.  We do this as a second pass since malformed CFG's can cause
  // dominator analysis constructure to crash and we want the verifier to be
  // resilient to malformed code.
  DominanceInfo theDomInfo(&fn);
  domInfo = &theDomInfo;
  for (auto &block : fn)
    if (failed(verifyDominance(block)))
      return failure();

  domInfo = nullptr;
  return success();
}

LogicalResult FuncVerifier::verifyBlock(Block &block, bool isTopLevel) {
  for (auto *arg : block.getArguments()) {
    if (arg->getOwner() != &block)
      return failure("block argument not owned by block", block);
  }

  // Verify that this block has a terminator.
  if (block.empty()) {
    return failure("block with no terminator", block);
  }

  // Verify the non-terminator operations separately so that we can verify
  // they has no successors.
  for (auto &op : llvm::make_range(block.begin(), std::prev(block.end()))) {
    if (op.getNumSuccessors() != 0)
      return failure(
          "operation with block successors must terminate its parent block",
          op);

    if (failed(verifyOperation(op)))
      return failure();
  }

  // Verify the terminator.
  if (failed(verifyOperation(block.back())))
    return failure();
  if (block.back().isKnownNonTerminator())
    return failure("block with no terminator", block);

  // Verify that this block is not branching to a block of a different
  // region.
  for (Block *successor : block.getSuccessors())
    if (successor->getParent() != block.getParent())
      return failure("branching to block of a different region", block.back());

  return success();
}

/// Check the invariants of the specified operation.
LogicalResult FuncVerifier::verifyOperation(Operation &op) {
  if (op.getFunction() != &fn)
    return failure("operation in the wrong function", op);

  // Check that operands are non-nil and structurally ok.
  for (auto *operand : op.getOperands()) {
    if (!operand)
      return failure("null operand found", op);

    if (operand->getFunction() != &fn)
      return failure("reference to operand defined in another function", op);
  }

  /// Verify that all of the attributes are okay.
  for (auto attr : op.getAttrs()) {
    if (!identifierRegex.match(attr.first))
      return failure("invalid attribute name '" + attr.first.strref() + "'",
                     op);

    // Check for any optional dialect specific attributes.
    if (!attr.first.strref().contains('.'))
      continue;
    if (auto *dialect = getDialectForAttribute(attr))
      if (failed(dialect->verifyOperationAttribute(&op, attr)))
        return failure();
  }

  // If we can get operation info for this, check the custom hook.
  auto *opInfo = op.getAbstractOperation();
  if (opInfo && failed(opInfo->verifyInvariants(&op)))
    return failure();

  // Verify that all child blocks are ok.
  for (auto &region : op.getRegions())
    for (auto &b : region)
      if (failed(verifyBlock(b, /*isTopLevel=*/false)))
        return failure();

  // If this is a registered operation, there is nothing left to do.
  if (opInfo)
    return success();

  // Otherwise, verify that the parent dialect allows un-registered operations.
  auto opName = op.getName().getStringRef();
  auto dialectPrefix = opName.split('.').first;

  // Check for an existing answer for the operation dialect.
  auto it = dialectAllowsUnknownOps.find(dialectPrefix);
  if (it == dialectAllowsUnknownOps.end()) {
    // If the operation dialect is registered, query it directly.
    if (auto *dialect = fn.getContext()->getRegisteredDialect(dialectPrefix))
      it = dialectAllowsUnknownOps
               .try_emplace(dialectPrefix, dialect->allowsUnknownOperations())
               .first;
    // Otherwise, conservatively allow unknown operations.
    else
      it = dialectAllowsUnknownOps.try_emplace(dialectPrefix, true).first;
  }

  if (!it->second) {
    return failure("unregistered operation '" + opName +
                       "' found in dialect ('" + dialectPrefix +
                       "') that does not allow unknown operations",
                   op);
  }

  return success();
}

LogicalResult FuncVerifier::verifyDominance(Block &block) {
  // Verify the dominance of each of the held operations.
  for (auto &op : block)
    if (failed(verifyOpDominance(op)))
      return failure();
  return success();
}

LogicalResult FuncVerifier::verifyOpDominance(Operation &op) {
  // Check that operands properly dominate this use.
  for (unsigned operandNo = 0, e = op.getNumOperands(); operandNo != e;
       ++operandNo) {
    auto *operand = op.getOperand(operandNo);
    if (domInfo->properlyDominates(operand, &op))
      continue;

    auto diag = op.emitError("operand #")
                << operandNo << " does not dominate this use";
    if (auto *useOp = operand->getDefiningOp())
      diag.attachNote(useOp->getLoc()) << "operand defined here";
    return failure();
  }

  // Verify the dominance of each of the nested blocks within this operation.
  for (auto &region : op.getRegions())
    for (auto &block : region)
      if (failed(verifyDominance(block)))
        return failure();

  return success();
}

//===----------------------------------------------------------------------===//
// Entrypoints
//===----------------------------------------------------------------------===//

/// Perform (potentially expensive) checks of invariants, used to detect
/// compiler bugs.  On error, this reports the error through the MLIRContext and
/// returns failure.
LogicalResult Function::verify() { return FuncVerifier(*this).verify(); }

/// Perform (potentially expensive) checks of invariants, used to detect
/// compiler bugs.  On error, this reports the error through the MLIRContext and
/// returns failure.
LogicalResult Module::verify() {
  /// Check that each function is correct.
  for (auto &fn : *this)
    if (failed(fn.verify()))
      return failure();

  return success();
}
