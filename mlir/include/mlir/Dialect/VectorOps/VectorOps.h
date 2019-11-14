//===- VectorOps.h - MLIR Super Vectorizer Operations -----------*- C++ -*-===//
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
// This file defines the Vector dialect.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_DIALECT_VECTOROPS_VECTOROPS_H
#define MLIR_DIALECT_VECTOROPS_VECTOROPS_H

#include "mlir/IR/Attributes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/StandardTypes.h"

namespace mlir {
namespace vector {

/// Dialect for Ops on higher-dimensional vector types.
class VectorOpsDialect : public Dialect {
public:
  VectorOpsDialect(MLIRContext *context);
  static StringRef getDialectNamespace() { return "vector"; }
};

#define GET_OP_CLASSES
#include "mlir/Dialect/VectorOps/VectorOps.h.inc"

} // end namespace vector
} // end namespace mlir

#endif // MLIR_DIALECT_VECTOROPS_VECTOROPS_H
