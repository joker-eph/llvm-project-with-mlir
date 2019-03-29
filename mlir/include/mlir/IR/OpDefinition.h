//===- OpDefinition.h - Classes for defining concrete Op types --*- C++ -*-===//
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
// This file implements helper classes for implementing the "Op" types.  This
// includes the Op type, which is the base class for Op class definitions,
// as well as number of traits in the OpTrait namespace that provide a
// declarative way to specify properties of Ops.
//
// The purpose of these types are to allow light-weight implementation of
// concrete ops (like DimOp) with very little boilerplate.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_IR_OPDEFINITION_H
#define MLIR_IR_OPDEFINITION_H

#include "mlir/IR/Operation.h"
#include "mlir/IR/SSAValue.h"
#include <type_traits>

namespace mlir {
class Builder;
class Type;
class OpAsmParser;
class OpAsmPrinter;
namespace OpTrait {
}

/// This type trait produces true if the specified type is in the specified
/// type list.
template <typename same, typename first, typename... more>
struct typelist_contains {
  static const bool value = std::is_same<same, first>::value ||
                            typelist_contains<same, more...>::value;
};
template <typename same, typename first>
struct typelist_contains<same, first> : std::is_same<same, first> {};

/// This pointer represents a notional "Operation*" but where the actual
/// storage of the pointer is maintained in the templated "OpType" class.
template <typename OpType>
class OpPointer {
public:
  explicit OpPointer() : value(Operation::getNull<OpType>().value) {}
  explicit OpPointer(OpType value) : value(value) {}

  OpType &operator*() { return value; }

  OpType *operator->() { return &value; }

  operator bool() const { return value.getOperation(); }

private:
  OpType value;
};

/// This pointer represents a notional "const Operation*" but where the actual
/// storage of the pointer is maintained in the templated "OpType" class.
template <typename OpType>
class ConstOpPointer {
public:
  explicit ConstOpPointer() : value(Operation::getNull<OpType>().value) {}
  explicit ConstOpPointer(OpType value) : value(value) {}

  const OpType &operator*() const { return value; }

  const OpType *operator->() const { return &value; }

  /// Return true if non-null.
  operator bool() const { return value.getOperation(); }

private:
  const OpType value;
};

/// This is the concrete base class that holds the operation pointer and has
/// non-generic methods that only depend on State (to avoid having them
/// instantiated on template types that don't affect them.
///
/// This also has the fallback implementations of customization hooks for when
/// they aren't customized.
class OpState {
public:
  /// Return the operation that this refers to.
  const Operation *getOperation() const { return state; }
  Operation *getOperation() { return state; }

  /// Return all of the attributes on this operation.
  ArrayRef<NamedAttribute> getAttrs() const { return state->getAttrs(); }

  /// Return an attribute with the specified name.
  Attribute *getAttr(StringRef name) const { return state->getAttr(name); }

  /// If the operation has an attribute of the specified type, return it.
  template <typename AttrClass>
  AttrClass *getAttrOfType(StringRef name) const {
    return dyn_cast_or_null<AttrClass>(getAttr(name));
  }

  /// If the an attribute exists with the specified name, change it to the new
  /// value.  Otherwise, add a new attribute with the specified name/value.
  void setAttr(Identifier name, Attribute *value) {
    state->setAttr(name, value);
  }

  /// Emit an error about fatal conditions with this operation, reporting up to
  /// any diagnostic handlers that may be listening.  NOTE: This may terminate
  /// the containing application, only use when the IR is in an inconsistent
  /// state.
  void emitError(const Twine &message) const;

  /// Emit an error with the op name prefixed, like "'dim' op " which is
  /// convenient for verifiers.  This always returns true.
  bool emitOpError(const Twine &message) const;

  /// Emit a warning about this operation, reporting up to any diagnostic
  /// handlers that may be listening.
  void emitWarning(const Twine &message) const;

  /// Emit a note about this operation, reporting up to any diagnostic
  /// handlers that may be listening.
  void emitNote(const Twine &message) const;

protected:
  // These are default implementations of customization hooks.

  /// If the concrete type didn't implement a custom verifier hook, just fall
  /// back to this one which accepts everything.
  bool verify() const { return false; }

  /// Unless overridden, the short form of an op is always rejected.  Op
  /// implementations should implement this to return boolean true on failure.
  /// On success, they should return false and fill in result with the fields to
  /// use.
  static bool parse(OpAsmParser *parser, OperationState *result);

  // The fallback for the printer is to print it the longhand form.
  void print(OpAsmPrinter *p) const;

  /// Mutability management is handled by the OpWrapper/OpConstWrapper classes,
  /// so we can cast it away here.
  explicit OpState(const Operation *state)
      : state(const_cast<Operation *>(state)) {}

private:
  Operation *state;
};

/// This template defines the constantFoldHook as used by AbstractOperation.
/// The default implementation uses a general constantFold method that can be
/// defined on custom ops which can return multiple results.
template <typename ConcreteType, bool isSingleResult, typename = void>
class ConstFoldingHook {
public:
  /// This hook implements a constant folder for this operation.  It returns
  /// true if folding failed, or returns false and fills in `results` on
  /// success.
  static bool constantFoldHook(const Operation *op,
                               ArrayRef<Attribute *> operands,
                               SmallVectorImpl<Attribute *> &results) {
    return op->getAs<ConcreteType>()->constantFold(operands, results,
                                                   op->getContext());
  }

  /// The fallback for the constant folder is to always fail to fold.
  ///
  bool constantFold(ArrayRef<Attribute *> operands,
                    SmallVectorImpl<Attribute *> &results,
                    MLIRContext *context) const {
    return true;
  }
};

/// This template specialization defines the constantFoldHook as used by
/// AbstractOperation for single-result operations.  This gives the hook a nicer
/// signature that is easier to implement.
template <typename ConcreteType, bool isSingleResult>
class ConstFoldingHook<ConcreteType, isSingleResult,
                       typename std::enable_if<isSingleResult>::type> {
public:
  /// This hook implements a constant folder for this operation.  It returns
  /// true if folding failed, or returns false and fills in `results` on
  /// success.
  static bool constantFoldHook(const Operation *op,
                               ArrayRef<Attribute *> operands,
                               SmallVectorImpl<Attribute *> &results) {
    auto *result =
        op->getAs<ConcreteType>()->constantFold(operands, op->getContext());
    if (!result)
      return true;

    results.push_back(result);
    return false;
  }
};

//===----------------------------------------------------------------------===//
// Operation Trait Types
//===----------------------------------------------------------------------===//

namespace OpTrait {

// These functions are out-of-line implementations of the methods in the
// corresponding trait classes.  This avoids them being template
// instantiated/duplicated.
namespace impl {
bool verifyZeroOperands(const Operation *op);
bool verifyOneOperand(const Operation *op);
bool verifyNOperands(const Operation *op, unsigned numOperands);
bool verifyAtLeastNOperands(const Operation *op, unsigned numOperands);
bool verifyZeroResult(const Operation *op);
bool verifyOneResult(const Operation *op);
bool verifyNResults(const Operation *op, unsigned numOperands);
bool verifyAtLeastNResults(const Operation *op, unsigned numOperands);
bool verifySameOperandsAndResult(const Operation *op);
bool verifyResultsAreFloatLike(const Operation *op);
bool verifyResultsAreIntegerLike(const Operation *op);
} // namespace impl

/// Helper class for implementing traits.  Clients are not expected to interact
/// with this directly, so its members are all protected.
template <typename ConcreteType, template <typename> class TraitType>
class TraitBase {
protected:
  /// Return the ultimate Operation being worked on.
  Operation *getOperation() {
    // We have to cast up to the trait type, then to the concrete type, then to
    // the BaseState class in explicit hops because the concrete type will
    // multiply derive from the (content free) TraitBase class, and we need to
    // be able to disambiguate the path for the C++ compiler.
    auto *trait = static_cast<TraitType<ConcreteType> *>(this);
    auto *concrete = static_cast<ConcreteType *>(trait);
    auto *base = static_cast<OpState *>(concrete);
    return base->getOperation();
  }
  const Operation *getOperation() const {
    return const_cast<TraitBase *>(this)->getOperation();
  }

  /// Provide default implementations of trait hooks.  This allows traits to
  /// provide exactly the overrides they care about.
  static bool verifyTrait(const Operation *op) { return false; }
};

/// This class provides the API for ops that are known to have exactly one
/// SSA operand.
template <typename ConcreteType>
class ZeroOperands : public TraitBase<ConcreteType, ZeroOperands> {
public:
  static bool verifyTrait(const Operation *op) {
    return impl::verifyZeroOperands(op);
  }

private:
  // Disable these.
  void getOperand() const {}
  void setOperand() const {}
};

/// This class provides the API for ops that are known to have exactly one
/// SSA operand.
template <typename ConcreteType>
class OneOperand : public TraitBase<ConcreteType, OneOperand> {
public:
  const SSAValue *getOperand() const {
    return this->getOperation()->getOperand(0);
  }

  SSAValue *getOperand() { return this->getOperation()->getOperand(0); }

  void setOperand(SSAValue *value) {
    this->getOperation()->setOperand(0, value);
  }

  static bool verifyTrait(const Operation *op) {
    return impl::verifyOneOperand(op);
  }
};

/// This class provides the API for ops that are known to have a specified
/// number of operands.  This is used as a trait like this:
///
///   class FooOp : public Op<FooOp, OpTrait::NOperands<2>::Impl> {
///
template <unsigned N> class NOperands {
public:
  template <typename ConcreteType>
  class Impl : public TraitBase<ConcreteType, NOperands<N>::Impl> {
  public:
    const SSAValue *getOperand(unsigned i) const {
      return this->getOperation()->getOperand(i);
    }

    SSAValue *getOperand(unsigned i) {
      return this->getOperation()->getOperand(i);
    }

    void setOperand(unsigned i, SSAValue *value) {
      this->getOperation()->setOperand(i, value);
    }

    static bool verifyTrait(const Operation *op) {
      return impl::verifyNOperands(op, N);
    }
  };
};

/// This class provides the API for ops that are known to have a at least a
/// specified number of operands.  This is used as a trait like this:
///
///   class FooOp : public Op<FooOp, OpTrait::AtLeastNOperands<2>::Impl> {
///
template <unsigned N> class AtLeastNOperands {
public:
  template <typename ConcreteType>
  class Impl : public TraitBase<ConcreteType, AtLeastNOperands<N>::Impl> {
  public:
    unsigned getNumOperands() const {
      return this->getOperation()->getNumOperands();
    }
    const SSAValue *getOperand(unsigned i) const {
      return this->getOperation()->getOperand(i);
    }

    SSAValue *getOperand(unsigned i) {
      return this->getOperation()->getOperand(i);
    }

    void setOperand(unsigned i, SSAValue *value) {
      this->getOperation()->setOperand(i, value);
    }

    // Support non-const operand iteration.
    using operand_iterator = Operation::operand_iterator;
    operand_iterator operand_begin() {
      return this->getOperation()->operand_begin();
    }
    operand_iterator operand_end() {
      return this->getOperation()->operand_end();
    }
    llvm::iterator_range<operand_iterator> getOperands() {
      return this->getOperation()->getOperands();
    }

    // Support const operand iteration.
    using const_operand_iterator = Operation::const_operand_iterator;
    const_operand_iterator operand_begin() const {
      return this->getOperation()->operand_begin();
    }
    const_operand_iterator operand_end() const {
      return this->getOperation()->operand_end();
    }
    llvm::iterator_range<const_operand_iterator> getOperands() const {
      return this->getOperation()->getOperands();
    }

    static bool verifyTrait(const Operation *op) {
      return impl::verifyAtLeastNOperands(op, N);
    }
  };
};

/// This class provides the API for ops which have an unknown number of
/// SSA operands.
template <typename ConcreteType>
class VariadicOperands : public TraitBase<ConcreteType, VariadicOperands> {
public:
  unsigned getNumOperands() const {
    return this->getOperation()->getNumOperands();
  }

  const SSAValue *getOperand(unsigned i) const {
    return this->getOperation()->getOperand(i);
  }

  SSAValue *getOperand(unsigned i) {
    return this->getOperation()->getOperand(i);
  }

  void setOperand(unsigned i, SSAValue *value) {
    this->getOperation()->setOperand(i, value);
  }

  // Support non-const operand iteration.
  using operand_iterator = Operation::operand_iterator;
  operand_iterator operand_begin() {
    return this->getOperation()->operand_begin();
  }
  operand_iterator operand_end() { return this->getOperation()->operand_end(); }
  llvm::iterator_range<operand_iterator> getOperands() {
    return this->getOperation()->getOperands();
  }

  // Support const operand iteration.
  using const_operand_iterator = Operation::const_operand_iterator;
  const_operand_iterator operand_begin() const {
    return this->getOperation()->operand_begin();
  }
  const_operand_iterator operand_end() const {
    return this->getOperation()->operand_end();
  }
  llvm::iterator_range<const_operand_iterator> getOperands() const {
    return this->getOperation()->getOperands();
  }
};

/// This class provides return value APIs for ops that are known to have a
/// zero results.
template <typename ConcreteType>
class ZeroResult : public TraitBase<ConcreteType, ZeroResult> {
public:
  static bool verifyTrait(const Operation *op) {
    return impl::verifyZeroResult(op);
  }
};

/// This class provides return value APIs for ops that are known to have a
/// single result.
template <typename ConcreteType>
class OneResult : public TraitBase<ConcreteType, OneResult> {
public:
  SSAValue *getResult() { return this->getOperation()->getResult(0); }
  const SSAValue *getResult() const {
    return this->getOperation()->getResult(0);
  }

  Type *getType() const { return getResult()->getType(); }

  static bool verifyTrait(const Operation *op) {
    return impl::verifyOneResult(op);
  }

  /// This hook implements a constant folder for this operation.  If the
  /// operation can be folded successfully, a non-null result is returned.  If
  /// not, a null pointer is returned.
  Attribute *constantFold(ArrayRef<Attribute *> operands,
                          MLIRContext *context) const {
    return nullptr;
  }
};

/// This class provides the API for ops that are known to have a specified
/// number of results.  This is used as a trait like this:
///
///   class FooOp : public Op<FooOp, OpTrait::NResults<2>::Impl> {
///
template <unsigned N> class NResults {
public:
  template <typename ConcreteType>
  class Impl : public TraitBase<ConcreteType, NResults<N>::Impl> {
  public:
    const SSAValue *getResult(unsigned i) const {
      return this->getOperation()->getResult(i);
    }

    SSAValue *getResult(unsigned i) {
      return this->getOperation()->getResult(i);
    }

    Type *getType(unsigned i) const { return getResult(i)->getType(); }

    static bool verifyTrait(const Operation *op) {
      return impl::verifyNResults(op, N);
    }
  };
};

/// This class provides the API for ops that are known to have at least a
/// specified number of results.  This is used as a trait like this:
///
///   class FooOp : public Op<FooOp, OpTrait::AtLeastNResults<2>::Impl> {
///
template <unsigned N> class AtLeastNResults {
public:
  template <typename ConcreteType>
  class Impl : public TraitBase<ConcreteType, AtLeastNResults<N>::Impl> {
  public:
    const SSAValue *getResult(unsigned i) const {
      return this->getOperation()->getResult(i);
    }

    SSAValue *getResult(unsigned i) {
      return this->getOperation()->getResult(i);
    }

    Type *getType(unsigned i) const { return getResult(i)->getType(); }

    static bool verifyTrait(const Operation *op) {
      return impl::verifyAtLeastNResults(op, N);
    }
  };
};

/// This class provides the API for ops which have an unknown number of
/// results.
template <typename ConcreteType>
class VariadicResults : public TraitBase<ConcreteType, VariadicResults> {
public:
  unsigned getNumResults() const {
    return this->getOperation()->getNumResults();
  }

  const SSAValue *getResult(unsigned i) const {
    return this->getOperation()->getResult(i);
  }

  SSAValue *getResult(unsigned i) { return this->getOperation()->getResult(i); }

  void setResult(unsigned i, SSAValue *value) {
    this->getOperation()->setResult(i, value);
  }
};

/// This class provides verification for ops that are known to have the same
/// operand and result type.
template <typename ConcreteType>
class SameOperandsAndResultType
    : public TraitBase<ConcreteType, SameOperandsAndResultType> {
public:
  static bool verifyTrait(const Operation *op) {
    return impl::verifySameOperandsAndResult(op);
  }
};

/// This class verifies that any results of the specified op have a floating
/// point type, a vector thereof, or a tensor thereof.
template <typename ConcreteType>
class ResultsAreFloatLike
    : public TraitBase<ConcreteType, ResultsAreFloatLike> {
public:
  static bool verifyTrait(const Operation *op) {
    return impl::verifyResultsAreFloatLike(op);
  }
};

/// This class verifies that any results of the specified op have an integer
/// type, a vector thereof, or a tensor thereof.
template <typename ConcreteType>
class ResultsAreIntegerLike
    : public TraitBase<ConcreteType, ResultsAreIntegerLike> {
public:
  static bool verifyTrait(const Operation *op) {
    return impl::verifyResultsAreIntegerLike(op);
  }
};

} // end namespace OpTrait

//===----------------------------------------------------------------------===//
// Operation Definition classes
//===----------------------------------------------------------------------===//

/// This provides public APIs that all operations should have.  The template
/// argument 'ConcreteType' should be the concrete type by CRTP and the others
/// are base classes by the policy pattern.
template <typename ConcreteType, template <typename T> class... Traits>
class Op : public OpState,
           public Traits<ConcreteType>...,
           public ConstFoldingHook<
               ConcreteType,
               typelist_contains<OpTrait::OneResult<ConcreteType>, OpState,
                                 Traits<ConcreteType>...>::value> {
public:
  /// Return the operation that this refers to.
  const Operation *getOperation() const { return OpState::getOperation(); }
  Operation *getOperation() { return OpState::getOperation(); }

  /// Return true if this "op class" can match against the specified operation.
  /// This hook can be overridden with a more specific implementation in
  /// the subclass of Base.
  ///
  static bool isClassFor(const Operation *op) {
    return op->getName().is(ConcreteType::getOperationName());
  }

  /// This is the hook used by the AsmParser to parse the custom form of this
  /// op from an .mlir file.  Op implementations should provide a parse method,
  /// which returns boolean true on failure.  On success, they should return
  /// false and fill in result with the fields to use.
  static bool parseAssembly(OpAsmParser *parser, OperationState *result) {
    return ConcreteType::parse(parser, result);
  }

  /// This is the hook used by the AsmPrinter to emit this to the .mlir file.
  /// Op implementations should provide a print method.
  static void printAssembly(const Operation *op, OpAsmPrinter *p) {
    op->getAs<ConcreteType>()->print(p);
  }

  /// This is the hook that checks whether or not this instruction is well
  /// formed according to the invariants of its opcode.  It delegates to the
  /// Traits for their policy implementations, and allows the user to specify
  /// their own verify() method.
  ///
  /// On success this returns false; on failure it emits an error to the
  /// diagnostic subsystem and returns true.
  static bool verifyInvariants(const Operation *op) {
    return BaseVerifier<Traits<ConcreteType>...>::verifyTrait(op) ||
           op->getAs<ConcreteType>()->verify();
  }

  // TODO: Provide a dump() method.

protected:
  explicit Op(const Operation *state) : OpState(state) {}

private:
  template <typename... Types> struct BaseVerifier;

  template <typename First, typename... Rest>
  struct BaseVerifier<First, Rest...> {
    static bool verifyTrait(const Operation *op) {
      return First::verifyTrait(op) || BaseVerifier<Rest...>::verifyTrait(op);
    }
  };

  template <typename First> struct BaseVerifier<First> {
    static bool verifyTrait(const Operation *op) {
      return First::verifyTrait(op);
    }
  };

  template <> struct BaseVerifier<> {
    static bool verifyTrait(const Operation *op) { return false; }
  };
};

// These functions are out-of-line implementations of the methods in BinaryOp,
// which avoids them being template instantiated/duplicated.
namespace impl {
void buildBinaryOp(Builder *builder, OperationState *result, SSAValue *lhs,
                   SSAValue *rhs);
bool parseBinaryOp(OpAsmParser *parser, OperationState *result);
void printBinaryOp(const Operation *op, OpAsmPrinter *p);
} // namespace impl

/// This template is used for operations that are simple binary ops that have
/// two input operands, one result, and whose operands and results all have
/// the same type.
///
/// From this structure, subclasses get a standard builder, parser and printer.
///
template <typename ConcreteType, template <typename T> class... Traits>
class BinaryOp
    : public Op<ConcreteType, OpTrait::NOperands<2>::Impl, OpTrait::OneResult,
                OpTrait::SameOperandsAndResultType, Traits...> {
public:
  static void build(Builder *builder, OperationState *result, SSAValue *lhs,
                    SSAValue *rhs) {
    impl::buildBinaryOp(builder, result, lhs, rhs);
  }
  static bool parse(OpAsmParser *parser, OperationState *result) {
    return impl::parseBinaryOp(parser, result);
  }
  void print(OpAsmPrinter *p) const {
    return impl::printBinaryOp(this->getOperation(), p);
  }

protected:
  explicit BinaryOp(const Operation *state)
      : Op<ConcreteType, OpTrait::NOperands<2>::Impl, OpTrait::OneResult,
           OpTrait::SameOperandsAndResultType, Traits...>(state) {}
};

} // end namespace mlir

#endif
