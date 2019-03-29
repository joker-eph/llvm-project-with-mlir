//===- Function.cpp - MLIR Function Classes -------------------------------===//
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

#include "mlir/IR/Function.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/Types.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
using namespace mlir;

Function::Function(Location location, StringRef name, FunctionType type,
                   ArrayRef<NamedAttribute> attrs)
    : name(Identifier::get(name, type.getContext())), location(location),
      type(type), attrs(type.getContext(), attrs),
      argAttrs(type.getNumInputs()), blocks(this) {}

Function::Function(Location location, StringRef name, FunctionType type,
                   ArrayRef<NamedAttribute> attrs,
                   ArrayRef<NamedAttributeList> argAttrs)
    : name(Identifier::get(name, type.getContext())), location(location),
      type(type), attrs(type.getContext(), attrs), argAttrs(argAttrs),
      blocks(this) {}

Function::~Function() {
  // Instructions may have cyclic references, which need to be dropped before we
  // can start deleting them.
  for (auto &bb : *this)
    bb.dropAllReferences();

  // Clean up function attributes referring to this function.
  FunctionAttr::dropFunctionReference(this);
}

MLIRContext *Function::getContext() const { return getType().getContext(); }

Module *llvm::ilist_traits<Function>::getContainingModule() {
  size_t Offset(
      size_t(&((Module *)nullptr->*Module::getSublistAccess(nullptr))));
  iplist<Function> *Anchor(static_cast<iplist<Function> *>(this));
  return reinterpret_cast<Module *>(reinterpret_cast<char *>(Anchor) - Offset);
}

/// This is a trait method invoked when a Function is added to a Module.  We
/// keep the module pointer and module symbol table up to date.
void llvm::ilist_traits<Function>::addNodeToList(Function *function) {
  assert(!function->getModule() && "already in a module!");
  auto *module = getContainingModule();
  function->module = module;

  // Add this function to the symbol table of the module, uniquing the name if
  // a conflict is detected.
  if (!module->symbolTable.insert({function->getName(), function}).second) {
    // If a conflict was detected, then the function will not have been added to
    // the symbol table.  Try suffixes until we get to a unique name that works.
    SmallString<128> nameBuffer(function->getName().begin(),
                                function->getName().end());
    unsigned originalLength = nameBuffer.size();

    // Iteratively try suffixes until we find one that isn't used.  We use a
    // module level uniquing counter to avoid N^2 behavior.
    do {
      nameBuffer.resize(originalLength);
      nameBuffer += '_';
      nameBuffer += std::to_string(module->uniquingCounter++);
      function->name = Identifier::get(nameBuffer, module->getContext());
    } while (
        !module->symbolTable.insert({function->getName(), function}).second);
  }
}

/// This is a trait method invoked when a Function is removed from a Module.
/// We keep the module pointer up to date.
void llvm::ilist_traits<Function>::removeNodeFromList(Function *function) {
  assert(function->module && "not already in a module!");

  // Remove the symbol table entry.
  function->module->symbolTable.erase(function->getName());
  function->module = nullptr;
}

/// This is a trait method invoked when an instruction is moved from one block
/// to another.  We keep the block pointer up to date.
void llvm::ilist_traits<Function>::transferNodesFromList(
    ilist_traits<Function> &otherList, function_iterator first,
    function_iterator last) {
  // If we are transferring functions within the same module, the Module
  // pointer doesn't need to be updated.
  Module *curParent = getContainingModule();
  if (curParent == otherList.getContainingModule())
    return;

  // Update the 'module' member and symbol table records for each function.
  for (; first != last; ++first) {
    removeNodeFromList(&*first);
    addNodeToList(&*first);
  }
}

/// Unlink this function from its Module and delete it.
void Function::erase() {
  assert(getModule() && "Function has no parent");
  getModule()->getFunctions().erase(this);
}

/// Emit a note about this instruction, reporting up to any diagnostic
/// handlers that may be listening.
void Function::emitNote(const Twine &message) const {
  getContext()->emitDiagnostic(getLoc(), message,
                               MLIRContext::DiagnosticKind::Note);
}

/// Emit a warning about this operation, reporting up to any diagnostic
/// handlers that may be listening.
void Function::emitWarning(const Twine &message) const {
  getContext()->emitDiagnostic(getLoc(), message,
                               MLIRContext::DiagnosticKind::Warning);
}

/// Emit an error about fatal conditions with this operation, reporting up to
/// any diagnostic handlers that may be listening.  This function always
/// returns true.  NOTE: This may terminate the containing application, only use
/// when the IR is in an inconsistent state.
bool Function::emitError(const Twine &message) const {
  return getContext()->emitError(getLoc(), message);
}

/// Clone the internal blocks from this function into dest and all attributes
/// from this function to dest.
void Function::cloneInto(Function *dest, BlockAndValueMapping &mapper) const {
  // Add the attributes of this function to dest.
  llvm::MapVector<Identifier, Attribute> newAttrs;
  for (auto &attr : dest->getAttrs())
    newAttrs.insert(attr);
  for (auto &attr : getAttrs()) {
    auto insertPair = newAttrs.insert(attr);

    // TODO(riverriddle) Verify that the two functions have compatible
    // attributes.
    (void)insertPair;
    assert((insertPair.second || insertPair.first->second == attr.second) &&
           "the two functions have incompatible attributes");
  }
  dest->setAttrs(newAttrs.takeVector());

  // Clone the block list.
  blocks.cloneInto(&dest->blocks, mapper, dest->getContext());
}

/// Create a deep copy of this function and all of its blocks, remapping
/// any operands that use values outside of the function using the map that is
/// provided (leaving them alone if no entry is present). Replaces references
/// to cloned sub-values with the corresponding value that is copied, and adds
/// those mappings to the mapper.
Function *Function::clone(BlockAndValueMapping &mapper) const {
  FunctionType newType = type;

  // If the function has a body, then the user might be deleting arguments to
  // the function by specifying them in the mapper. If so, we don't add the
  // argument to the input type vector.
  bool isExternalFn = isExternal();
  if (!isExternalFn) {
    SmallVector<Type, 4> inputTypes;
    for (unsigned i = 0, e = getNumArguments(); i != e; ++i)
      if (!mapper.contains(getArgument(i)))
        inputTypes.push_back(type.getInput(i));
    newType = FunctionType::get(inputTypes, type.getResults(), getContext());
  }

  // Create the new function.
  Function *newFunc = new Function(getLoc(), getName(), newType);

  /// Set the argument attributes for arguments that aren't being replaced.
  for (unsigned i = 0, e = getNumArguments(), destI = 0; i != e; ++i)
    if (isExternalFn || !mapper.contains(getArgument(i)))
      newFunc->setArgAttrs(destI++, getArgAttrs(i));

  /// Clone the current function into the new one and return it.
  cloneInto(newFunc, mapper);
  return newFunc;
}
Function *Function::clone() const {
  BlockAndValueMapping mapper;
  return clone(mapper);
}

//===----------------------------------------------------------------------===//
// Function implementation.
//===----------------------------------------------------------------------===//

/// Add an entry block to an empty function, and set up the block arguments
/// to match the signature of the function.
void Function::addEntryBlock() {
  assert(empty() && "function already has an entry block");
  auto *entry = new Block();
  push_back(entry);
  entry->addArguments(type.getInputs());
}

void Function::walk(const std::function<void(Instruction *)> &callback) {
  // Walk each of the blocks within the function.
  for (auto &block : getBlocks())
    block.walk(callback);
}

void Function::walkPostOrder(
    const std::function<void(Instruction *)> &callback) {
  // Walk each of the blocks within the function.
  for (auto &block : llvm::reverse(getBlocks()))
    block.walkPostOrder(callback);
}
