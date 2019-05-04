//===- LoadStoreOps.h - Linalg dialect Load/Store operation definitions ---===//
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

#ifndef LINALG3_LOADSTOREOP_H_
#define LINALG3_LOADSTOREOP_H_

#include "mlir/IR/OpDefinition.h"
#include "mlir/Support/LLVM.h"

namespace linalg {

class ViewType;

/// A linalg.LoadOp is the counterpart of affine.load but operating on ViewType
/// instead of MemRefType.
class LoadOp : public mlir::Op<LoadOp, mlir::OpTrait::VariadicOperands,
                               mlir::OpTrait::OneResult> {
public:
  friend mlir::Operation;
  using Op::Op;

  //////////////////////////////////////////////////////////////////////////////
  // Hooks to customize the behavior of this op.
  //////////////////////////////////////////////////////////////////////////////
  static llvm::StringRef getOperationName() { return "linalg.load"; }
  static void build(mlir::Builder *b, mlir::OperationState *result,
                    mlir::Value *view,
                    mlir::ArrayRef<mlir::Value *> indices = {});
  mlir::LogicalResult verify();
  static bool parse(mlir::OpAsmParser *parser, mlir::OperationState *result);
  void print(mlir::OpAsmPrinter *p);

  //////////////////////////////////////////////////////////////////////////////
  // Op-specific functionality.
  //////////////////////////////////////////////////////////////////////////////
  unsigned getRank();
  ViewType getViewType();
  mlir::Value *getView() { return getOperand(0); }
  mlir::Operation::operand_range getIndices() {
    return {operand_begin() + 1, operand_end()};
  }
};

/// A linalg.StoreOp is the counterpart of affine.store but operating on
/// ViewType instead of MemRefType.
class StoreOp : public mlir::Op<StoreOp, mlir::OpTrait::VariadicOperands,
                                mlir::OpTrait::ZeroResult> {
public:
  friend mlir::Operation;
  using Op::Op;

  //////////////////////////////////////////////////////////////////////////////
  // Hooks to customize the behavior of this op.
  //////////////////////////////////////////////////////////////////////////////
  static llvm::StringRef getOperationName() { return "linalg.store"; }
  static void build(mlir::Builder *b, mlir::OperationState *result,
                    mlir::Value *valueToStore, mlir::Value *view,
                    mlir::ArrayRef<mlir::Value *> indices = {});
  mlir::LogicalResult verify();
  static bool parse(mlir::OpAsmParser *parser, mlir::OperationState *result);
  void print(mlir::OpAsmPrinter *p);

  //////////////////////////////////////////////////////////////////////////////
  // Op-specific functionality.
  //////////////////////////////////////////////////////////////////////////////
  unsigned getRank();
  ViewType getViewType();
  mlir::Value *getValueToStore() { return getOperand(0); }
  mlir::Value *getView() { return getOperand(1); }
  mlir::Operation::operand_range getIndices() {
    return {operand_begin() + 2, operand_end()};
  }
};

} // namespace linalg

#endif // LINALG3_LOADSTOREOP_H_
