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

#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {
class Any;
} // end namespace llvm

namespace mlir {
class AnalysisManager;
class MLIRContext;
class ModuleOp;
class OperationName;
class Operation;
class Pass;
class PassInstrumentation;
class PassInstrumentor;

namespace detail {
struct OpPassManagerImpl;
} // end namespace detail

//===----------------------------------------------------------------------===//
// OpPassManager
//===----------------------------------------------------------------------===//

/// This class represents a pass manager that runs passes on a specific
/// operation type. This class is not constructed directly, but nested within
/// other OpPassManagers or the top-level PassManager.
class OpPassManager {
public:
  OpPassManager(OpPassManager &&) = default;
  OpPassManager(const OpPassManager &rhs);
  ~OpPassManager();
  OpPassManager &operator=(const OpPassManager &rhs);

  /// Run the held passes over the given operation.
  LogicalResult run(Operation *op, AnalysisManager am);

  /// Nest a new operation pass manager for the given operation kind under this
  /// pass manager.
  OpPassManager &nest(const OperationName &nestedName);
  OpPassManager &nest(StringRef nestedName);
  template <typename OpT> OpPassManager &nest() {
    return nest(OpT::getOperationName());
  }

  /// Add the given pass to this pass manager. The pass must either be an opaque
  /// `OperationPass`, or an `OpPass` that operates on operations of the same
  /// type as this pass manager.
  void addPass(std::unique_ptr<Pass> pass);

  /// Returns the number of passes held by this manager.
  size_t size() const;

  /// Return an instance of the context.
  MLIRContext *getContext() const;

  /// Return the operation name that this pass manager operates on.
  const OperationName &getOpName() const;

  /// Returns the internal implementation instance.
  detail::OpPassManagerImpl &getImpl();

private:
  OpPassManager(OperationName name, bool disableThreads, bool verifyPasses);

  /// A pointer to an internal implementation instance.
  std::unique_ptr<detail::OpPassManagerImpl> impl;

  /// Allow access to the constructor.
  friend class PassManager;
};

//===----------------------------------------------------------------------===//
// PassManager
//===----------------------------------------------------------------------===//

/// An enum describing the different display modes for the pass timing
/// information within the pass manager.
enum class PassTimingDisplayMode {
  // In this mode the results are displayed in a list sorted by total time,
  // with each pass/analysis instance aggregated into one unique result.
  List,

  // In this mode the results are displayed in a nested pipeline view that
  // mirrors the internal pass pipeline that is being executed in the pass
  // manager.
  Pipeline,
};

/// The main pass manager and pipeline builder.
class PassManager {
public:
  // If verifyPasses is true, the verifier is run after each pass.
  PassManager(MLIRContext *ctx, bool verifyPasses = true);
  ~PassManager();

  /// Run the passes within this manager on the provided module.
  LLVM_NODISCARD
  LogicalResult run(ModuleOp module);

  /// Disable support for multi-threading within the pass manager.
  void disableMultithreading(bool disable = true);

  //===--------------------------------------------------------------------===//
  // Pipeline Building
  //===--------------------------------------------------------------------===//

  /// Allow converting to the impl OpPassManager.
  operator OpPassManager &() { return opPassManager; }

  /// Add an opaque pass pointer to the current manager. This takes ownership
  /// over the provided pass pointer.
  void addPass(std::unique_ptr<Pass> pass);

  /// Allow nesting other operation pass managers.
  OpPassManager &nest(const OperationName &nestedName) {
    return opPassManager.nest(nestedName);
  }
  template <typename OpT> OpPassManager &nest() {
    return opPassManager.nest<OpT>();
  }

  //===--------------------------------------------------------------------===//
  // Instrumentations
  //===--------------------------------------------------------------------===//

  /// Add the provided instrumentation to the pass manager. This takes ownership
  /// over the given pointer.
  void addInstrumentation(PassInstrumentation *pi);

  /// Add an instrumentation to print the IR before and after pass execution.
  /// * 'shouldPrintBeforePass' and 'shouldPrintAfterPass' correspond to filter
  ///   functions that take a 'Pass *'. These function should return true if the
  ///   IR should be printed or not.
  /// * 'printModuleScope' signals if the module IR should be printed, even for
  ///   non module passes.
  /// * 'out' corresponds to the stream to output the printed IR to.
  void enableIRPrinting(std::function<bool(Pass *)> shouldPrintBeforePass,
                        std::function<bool(Pass *)> shouldPrintAfterPass,
                        bool printModuleScope, raw_ostream &out);

  /// Add an instrumentation to time the execution of passes and the computation
  /// of analyses.
  /// Note: Timing should be enabled after all other instrumentations to avoid
  /// any potential "ghost" timing from other instrumentations being
  /// unintentionally included in the timing results.
  void enableTiming(
      PassTimingDisplayMode displayMode = PassTimingDisplayMode::Pipeline);

private:
  /// The top level pass manager instance.
  OpPassManager opPassManager;

  /// Flag that specifies if pass timing is enabled.
  bool passTiming : 1;

  /// A manager for pass instrumentations.
  std::unique_ptr<PassInstrumentor> instrumentor;
};

/// Register a set of useful command-line options that can be used to configure
/// a pass manager. The values of these options can be applied via the
/// 'applyPassManagerCLOptions' method below.
void registerPassManagerCLOptions();

/// Apply any values provided to the pass manager options that were registered
/// with 'registerPassManagerOptions'.
void applyPassManagerCLOptions(PassManager &pm);
} // end namespace mlir

#endif // MLIR_PASS_PASSMANAGER_H
