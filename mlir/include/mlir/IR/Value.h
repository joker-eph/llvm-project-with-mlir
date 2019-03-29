//===- Value.h - Base of the SSA Value hierarchy ----------------*- C++ -*-===//
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
// This file defines generic Value type and manipulation utilities.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_IR_VALUE_H
#define MLIR_IR_VALUE_H

#include "mlir/IR/Types.h"
#include "mlir/IR/UseDefLists.h"
#include "mlir/Support/LLVM.h"

namespace mlir {
class Block;
class Function;
class OperationInst;
class Statement;
class Value;
using Instruction = Statement;

/// Operands contain a Value.
using InstOperand = IROperandImpl<Value, Statement>;

/// This is the common base class for all SSA values in the MLIR system,
/// representing a computable value that has a type and a set of users.
///
class Value : public IRObjectWithUseList {
public:
  /// This enumerates all of the SSA value kinds in the MLIR system.
  enum class Kind {
    BlockArgument, // block argument
    InstResult,    // operation instruction result
    ForStmt,       // 'for' statement induction variable
  };

  ~Value() {}

  Kind getKind() const { return typeAndKind.getInt(); }

  Type getType() const { return typeAndKind.getPointer(); }

  /// Replace all uses of 'this' value with the new value, updating anything in
  /// the IR that uses 'this' to use the other value instead.  When this returns
  /// there are zero uses of 'this'.
  void replaceAllUsesWith(Value *newValue) {
    IRObjectWithUseList::replaceAllUsesWith(newValue);
  }

  /// TODO: move isValidDim/isValidSymbol to a utility library specific to the
  /// polyhedral operations.

  /// Returns true if the given Value can be used as a dimension id.
  bool isValidDim() const;

  /// Returns true if the given Value can be used as a symbol.
  bool isValidSymbol() const;

  /// Return the function that this Value is defined in.
  Function *getFunction();

  /// Return the function that this Value is defined in.
  const Function *getFunction() const {
    return const_cast<Value *>(this)->getFunction();
  }

  /// If this value is the result of an operation, return the instruction
  /// that defines it.
  OperationInst *getDefiningInst();
  const OperationInst *getDefiningInst() const {
    return const_cast<Value *>(this)->getDefiningInst();
  }

  using use_iterator = ValueUseIterator<InstOperand, Statement>;
  using use_range = llvm::iterator_range<use_iterator>;

  inline use_iterator use_begin() const;
  inline use_iterator use_end() const;

  /// Returns a range of all uses, which is useful for iterating over all uses.
  inline use_range getUses() const;

  void print(raw_ostream &os) const;
  void dump() const;

protected:
  Value(Kind kind, Type type) : typeAndKind(type, kind) {}

private:
  const llvm::PointerIntPair<Type, 3, Kind> typeAndKind;
};

inline raw_ostream &operator<<(raw_ostream &os, const Value &value) {
  value.print(os);
  return os;
}

// Utility functions for iterating through Value uses.
inline auto Value::use_begin() const -> use_iterator {
  return use_iterator((InstOperand *)getFirstUse());
}

inline auto Value::use_end() const -> use_iterator {
  return use_iterator(nullptr);
}

inline auto Value::getUses() const -> llvm::iterator_range<use_iterator> {
  return {use_begin(), use_end()};
}

/// Block arguments are values.
class BlockArgument : public Value {
public:
  static bool classof(const Value *value) {
    return value->getKind() == Kind::BlockArgument;
  }

  /// Return the function that this argument is defined in.
  Function *getFunction();
  const Function *getFunction() const {
    return const_cast<BlockArgument *>(this)->getFunction();
  }

  Block *getOwner() { return owner; }
  const Block *getOwner() const { return owner; }

private:
  friend class Block; // For access to private constructor.
  BlockArgument(Type type, Block *owner)
      : Value(Value::Kind::BlockArgument, type), owner(owner) {}

  /// The owner of this operand.
  /// TODO: can encode this more efficiently to avoid the space hit of this
  /// through bitpacking shenanigans.
  Block *const owner;
};

/// This is a value defined by a result of an operation instruction.
class InstResult : public Value {
public:
  InstResult(Type type, OperationInst *owner)
      : Value(Value::Kind::InstResult, type), owner(owner) {}

  static bool classof(const Value *value) {
    return value->getKind() == Kind::InstResult;
  }

  OperationInst *getOwner() { return owner; }
  const OperationInst *getOwner() const { return owner; }

  /// Returns the number of this result.
  unsigned getResultNumber() const;

private:
  /// The owner of this operand.
  /// TODO: can encode this more efficiently to avoid the space hit of this
  /// through bitpacking shenanigans.
  OperationInst *const owner;
};

} // namespace mlir

#endif
