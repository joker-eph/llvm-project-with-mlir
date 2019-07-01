//===- Module.h - MLIR Module Class -----------------------------*- C++ -*-===//
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
// Module is the top-level container for code in an MLIR program.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_IR_MODULE_H
#define MLIR_IR_MODULE_H

#include "mlir/IR/Function.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/ADT/ilist.h"

namespace mlir {

class Module {
public:
  explicit Module(MLIRContext *context) : context(context) {}

  MLIRContext *getContext() { return context; }

  /// An iterator class used to iterate over the held functions.
  class iterator : public llvm::mapped_iterator<
                       llvm::iplist<detail::FunctionStorage>::iterator,
                       Function (*)(detail::FunctionStorage &)> {
    static Function unwrap(detail::FunctionStorage &impl) { return &impl; }

  public:
    using reference = Function;

    /// Initializes the operand type iterator to the specified operand iterator.
    iterator(llvm::iplist<detail::FunctionStorage>::iterator it)
        : llvm::mapped_iterator<llvm::iplist<detail::FunctionStorage>::iterator,
                                Function (*)(detail::FunctionStorage &)>(
              it, &unwrap) {}
    iterator(Function it)
        : iterator(llvm::iplist<detail::FunctionStorage>::iterator(it.impl)) {}
  };

  /// This is the list of functions in the module.
  llvm::iterator_range<iterator> getFunctions() { return {begin(), end()}; }

  // Iteration over the functions in the module.
  iterator begin() { return functions.begin(); }
  iterator end() { return functions.end(); }
  Function front() { return &functions.front(); }
  Function back() { return &functions.back(); }

  void push_back(Function fn) { functions.push_back(fn.impl); }
  void insert(iterator insertPt, Function fn) {
    functions.insert(insertPt.getCurrent(), fn.impl);
  }

  // Interfaces for working with the symbol table.

  /// Look up a function with the specified name, returning null if no such
  /// name exists. Function names never include the @ on them. Note: This
  /// performs a linear scan of held symbols.
  Function getNamedFunction(StringRef name) {
    return getNamedFunction(Identifier::get(name, getContext()));
  }

  /// Look up a function with the specified name, returning null if no such
  /// name exists. Function names never include the @ on them. Note: This
  /// performs a linear scan of held symbols.
  Function getNamedFunction(Identifier name) {
    auto it = llvm::find_if(functions, [name](detail::FunctionStorage &fn) {
      return Function(&fn).getName() == name;
    });
    return it == functions.end() ? nullptr : &*it;
  }

  /// Perform (potentially expensive) checks of invariants, used to detect
  /// compiler bugs.  On error, this reports the error through the MLIRContext
  /// and returns failure.
  LogicalResult verify();

  void print(raw_ostream &os);
  void dump();

private:
  friend struct llvm::ilist_traits<detail::FunctionStorage>;
  friend detail::FunctionStorage;
  friend Function;

  /// getSublistAccess() - Returns pointer to member of function list
  static llvm::iplist<detail::FunctionStorage> Module::*
  getSublistAccess(detail::FunctionStorage *) {
    return &Module::functions;
  }

  /// The context attached to this module.
  MLIRContext *context;

  /// This is the actual list of functions the module contains.
  llvm::iplist<detail::FunctionStorage> functions;
};

/// A class used to manage the symbols held by a module. This class handles
/// ensures that symbols inserted into a module have a unique name, and provides
/// efficent named lookup to held symbols.
class ModuleManager {
public:
  ModuleManager(Module *module) : module(module), symbolTable(module) {}

  /// Look up a symbol with the specified name, returning null if no such
  /// name exists. Names must never include the @ on them.
  template <typename NameTy> Function getNamedFunction(NameTy &&name) const {
    return symbolTable.lookup(name);
  }

  /// Insert a new symbol into the module, auto-renaming it as necessary.
  void insert(Function function) {
    symbolTable.insert(function);
    module->push_back(function);
  }
  void insert(Module::iterator insertPt, Function function) {
    symbolTable.insert(function);
    module->insert(insertPt, function);
  }

  /// Remove the given symbol from the module symbol table and then erase it.
  void erase(Function function) {
    symbolTable.erase(function);
    function.erase();
  }

  /// Return the internally held module.
  Module *getModule() const { return module; }

  /// Return the context of the internal module.
  MLIRContext *getContext() const { return module->getContext(); }

private:
  Module *module;
  SymbolTable symbolTable;
};

//===--------------------------------------------------------------------===//
// Module Operation.
//===--------------------------------------------------------------------===//

/// ModuleOp represents a module, or an operation containing one region with a
/// single block containing opaque operations. A ModuleOp contains a symbol
/// table for operations, like FuncOp, held within its region. The region of a
/// module is not allowed to implicitly capture global values, and all external
/// references must use attributes.
class ModuleOp : public Op<ModuleOp, OpTrait::ZeroOperands, OpTrait::ZeroResult,
                           OpTrait::IsIsolatedFromAbove> {
public:
  using Op::Op;
  static StringRef getOperationName() { return "module"; }

  static void build(Builder *builder, OperationState *result);

  /// Operation hooks.
  static ParseResult parse(OpAsmParser *parser, OperationState *result);
  void print(OpAsmPrinter *p);
  LogicalResult verify();

  /// Return body of this module.
  Block *getBody();
};

/// The ModuleTerminatorOp is a special terminator operation for the body of a
/// ModuleOp, it has no semantic meaning beyond keeping the body of a ModuleOp
/// well-formed.
///
/// This operation does _not_ have a custom syntax. However, ModuleOp will omit
/// the terminator in their custom syntax for brevity.
class ModuleTerminatorOp
    : public Op<ModuleTerminatorOp, OpTrait::ZeroOperands, OpTrait::ZeroResult,
                OpTrait::IsTerminator> {
public:
  using Op::Op;
  static StringRef getOperationName() { return "module_terminator"; }

  static void build(Builder *, OperationState *) {}
  LogicalResult verify();
};

} // end namespace mlir

#endif // MLIR_IR_MODULE_H
