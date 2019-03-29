//===- Dialect.h - IR Dialect Description -----------------------*- C++ -*-===//
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
// This file defines the 'dialect' abstraction.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_IR_DIALECT_H
#define MLIR_IR_DIALECT_H

#include "mlir/IR/OperationSupport.h"

namespace mlir {

/// Dialects are groups of MLIR operations and behavior associated with the
/// entire group.  For example, hooks into other systems for constant folding,
/// default named types for asm printing, etc.
///
/// Instances of the dialect object are global across all MLIRContext's that may
/// be active in the process.
///
class Dialect {
public:
  MLIRContext *getContext() const { return context; }

  StringRef getOperationPrefix() const { return opPrefix; }

  /// Dialect implementations can implement this hook. It should attempt to
  /// constant fold this operation with the specified constant operand values -
  /// the elements in "operands" will correspond directly to the operands of the
  /// operation, but may be null if non-constant.  If constant folding is
  /// successful, this returns false and fills in the `results` vector.  If not,
  /// this returns true and `results` is unspecified.
  ///
  /// If not overridden, this fallback implementation always fails to fold.
  ///
  virtual bool constantFold(const Operation *op, ArrayRef<Attribute> operands,
                            SmallVectorImpl<Attribute> &results) const {
    return true;
  }

  // TODO: Hook to return the list of named types that are known.

  // TODO: Hook to return list of dialect defined types, like tf_control.

  virtual ~Dialect();

protected:
  /// The prefix should be common across all ops in this set, e.g. "" for the
  /// standard operation set, and "tf." for the TensorFlow ops like "tf.add".
  Dialect(StringRef opPrefix, MLIRContext *context);

  /// This method is used by derived classes to add their operations to the set.
  ///
  template <typename... Args> void addOperations() {
    VariadicOperationAdder<Args...>::addToSet(*this);
  }

  // It would be nice to define this as variadic functions instead of a nested
  // variadic type, but we can't do that: function template partial
  // specialization is not allowed, and we can't define an overload set because
  // we don't have any arguments of the types we are pushing around.
  template <typename First, typename... Rest> class VariadicOperationAdder {
  public:
    static void addToSet(Dialect &dialect) {
      dialect.addOperation(AbstractOperation::get<First>(dialect));
      VariadicOperationAdder<Rest...>::addToSet(dialect);
    }
  };

  template <typename First> class VariadicOperationAdder<First> {
  public:
    static void addToSet(Dialect &dialect) {
      dialect.addOperation(AbstractOperation::get<First>(dialect));
    }
  };

  void addOperation(AbstractOperation opInfo);

private:
  Dialect(const Dialect &) = delete;
  void operator=(Dialect &) = delete;

  /// Register this dialect object with the specified context.  The context
  /// takes ownership of the heap allocated dialect.
  void registerDialect(MLIRContext *context);

  /// This is the prefix that all operations belonging to this operation set
  /// start with.
  StringRef opPrefix;

  /// This is the context that owns this Dialect object.
  MLIRContext *context;
};

using DialectAllocatorFunction = std::function<void(MLIRContext *)>;

/// Register a specific dialect creation function with the system, typically
/// used through the DialectRegistration template.
void registerDialectAllocator(const DialectAllocatorFunction &function);

/// Registers all dialects with the specified MLIRContext.
void registerAllDialects(MLIRContext *context);

/// DialectRegistration provides a global initialiser that registers a Dialect
/// allocation routine.
///
/// Usage:
///
///   // At namespace scope.
///   static DialectRegistration<MyDialect> Unused;
template <typename ConcreteDialect> struct DialectRegistration {
  DialectRegistration() {
    registerDialectAllocator([&](MLIRContext *ctx) {
      // Just allocate the dialect, the context takes ownership of it.
      new ConcreteDialect(ctx);
    });
  }
};

} // namespace mlir

#endif
