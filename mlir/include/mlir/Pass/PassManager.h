//===- PassManager.h - Pass Management Interface ----------------*- C++ -*-===//
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

#ifndef MLIR_PASS_PASSMANAGER_H
#define MLIR_PASS_PASSMANAGER_H

#include "mlir/Pass/Pass.h"
#include "llvm/ADT/SmallVector.h"

namespace mlir {
namespace detail {
/// The abstract base pass executor class.
class PassExecutor {
public:
  enum Kind { FunctionExecutor, ModuleExecutor };
  explicit PassExecutor(Kind kind) : kind(kind) {}

  /// Get the kind of this executor.
  Kind getKind() const { return kind; }

private:
  /// The kind of executor this object is.
  Kind kind;
};

/// A pass executor that contains a list of passes over a function.
class FunctionPassExecutor : public PassExecutor {
public:
  FunctionPassExecutor() : PassExecutor(Kind::FunctionExecutor) {}
  FunctionPassExecutor(FunctionPassExecutor &&) = default;

  // TODO(riverriddle) Allow copying.
  FunctionPassExecutor(const FunctionPassExecutor &) = delete;
  FunctionPassExecutor &operator=(const FunctionPassExecutor &) = delete;

  /// Run the executor on the given function.
  bool run(Function *function);

  /// Add a pass to the current executor. This takes ownership over the provided
  /// pass pointer.
  void addPass(FunctionPassBase *pass) { passes.emplace_back(pass); }

  static bool classof(const PassExecutor *pe) {
    return pe->getKind() == Kind::FunctionExecutor;
  }

private:
  std::vector<std::unique_ptr<FunctionPassBase>> passes;
};

/// A pass executor that contains a list of passes over a module unit.
class ModulePassExecutor : public PassExecutor {
public:
  ModulePassExecutor() : PassExecutor(Kind::ModuleExecutor) {}
  ModulePassExecutor(ModulePassExecutor &&) = default;

  // Don't allow copying.
  ModulePassExecutor(const ModulePassExecutor &) = delete;
  ModulePassExecutor &operator=(const ModulePassExecutor &) = delete;

  /// Run the executor on the given module.
  bool run(Module *module);

  /// Add a pass to the current executor. This takes ownership over the provided
  /// pass pointer.
  void addPass(ModulePassBase *pass) { passes.emplace_back(pass); }

  static bool classof(const PassExecutor *pe) {
    return pe->getKind() == Kind::ModuleExecutor;
  }

private:
  /// Set of passes to run on the given module.
  std::vector<std::unique_ptr<ModulePassBase>> passes;
};

/// An adaptor module pass used to run function passes over all of the
/// non-external functions of a module.
class ModuleToFunctionPassAdaptor
    : public ModulePass<ModuleToFunctionPassAdaptor> {
public:
  ModuleToFunctionPassAdaptor() = default;
  ModuleToFunctionPassAdaptor(ModuleToFunctionPassAdaptor &&) = default;

  // TODO(riverriddle) Allow copying.
  ModuleToFunctionPassAdaptor(const ModuleToFunctionPassAdaptor &) = delete;
  ModuleToFunctionPassAdaptor &
  operator=(const ModuleToFunctionPassAdaptor &) = delete;

  /// run the held function pipeline over all non-external functions within the
  /// module.
  PassResult runOnModule() override;

  /// Returns the function pass executor for this adaptor.
  FunctionPassExecutor &getFunctionExecutor() { return fpe; }

private:
  FunctionPassExecutor fpe;
};
} // end namespace detail

/// The main pass manager and pipeline builder.
class PassManager {
public:
  /// Add an opaque pass pointer to the current manager. This takes ownership
  /// over the provided pass pointer.
  void addPass(Pass *pass) {
    switch (pass->getKind()) {
    case Pass::Kind::FunctionPass:
      addPass(cast<FunctionPassBase>(pass));
      break;
    case Pass::Kind::ModulePass:
      addPass(cast<ModulePassBase>(pass));
      break;
    }
  }

  /// Add a module pass to the current manager. This takes ownership over the
  /// provided pass pointer.
  void addPass(ModulePassBase *pass) {
    nestedExecutorStack.clear();
    mpe.addPass(pass);
  }

  /// Add a function pass to the current manager. This takes ownership over the
  /// provided pass pointer. This will automatically create a function pass
  /// executor if necessary.
  void addPass(FunctionPassBase *pass) {
    detail::FunctionPassExecutor *fpe;
    if (nestedExecutorStack.empty()) {
      /// Create an executor adaptor for this pass.
      auto *adaptor = new detail::ModuleToFunctionPassAdaptor();
      mpe.addPass(adaptor);

      /// Add the executor to the stack.
      fpe = &adaptor->getFunctionExecutor();
      nestedExecutorStack.push_back(fpe);
    } else {
      fpe = cast<detail::FunctionPassExecutor>(nestedExecutorStack.back());
    }
    fpe->addPass(pass);
  }

  /// Run the passes within this manager on the provided module.
  bool run(Module *module) { return mpe.run(module); }

private:
  /// A stack of nested pass executors on sub-module IR units, e.g. function.
  llvm::SmallVector<detail::PassExecutor *, 1> nestedExecutorStack;

  /// The top level module pass executor.
  detail::ModulePassExecutor mpe;
};

} // end namespace mlir

#endif // MLIR_PASS_PASSMANAGER_H
