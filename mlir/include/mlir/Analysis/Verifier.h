//===- Verifier.h - Verifier analysis for MLIR structures -------*- C++ -*-===//
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

#ifndef MLIR_ANALYSIS_VERIFIER_H
#define MLIR_ANALYSIS_VERIFIER_H

namespace mlir {
class Function;
class LogicalResult;
class Module;
class Operation;

/// Perform (potentially expensive) checks of invariants, used to detect
/// compiler bugs, on this operation and any nested operations. On error, this
/// reports the error through the MLIRContext and returns failure.
LogicalResult verify(Operation *op);

/// Perform (potentially expensive) checks of invariants, used to detect
/// compiler bugs, on this IR unit and any nested below. On error, this
/// reports the error through the MLIRContext and returns failure.
LogicalResult verify(Function fn);
LogicalResult verify(Module module);
} //  end namespace mlir

#endif
