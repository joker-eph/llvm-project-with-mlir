//===- ComposeAffineMaps.cpp - MLIR Affine Transform Class-----*- C++ -*-===//
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
// This file implements a testing pass which composes affine maps from
// AffineApplyOps in an MLFunction, by forward subtituting results from an
// AffineApplyOp into any of its users which are also AffineApplyOps.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/StmtVisitor.h"
#include "mlir/StandardOps/StandardOps.h"
#include "mlir/Transforms/Pass.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Transforms/Utils.h"
#include "llvm/Support/CommandLine.h"

using namespace mlir;

namespace {

// ComposeAffineMaps walks stmt blocks in an MLFunction, and for each
// AffineApplyOp, forward substitutes its results into any users which are
// also AffineApplyOps. After forward subtituting its results, AffineApplyOps
// with no remaining uses are collected and erased after the walk.
// TODO(andydavis) Remove this when Chris adds instruction combiner pass.
struct ComposeAffineMaps : public MLFunctionPass,
                           StmtWalker<ComposeAffineMaps> {
  std::vector<OperationStmt *> affineApplyOpsToErase;

  explicit ComposeAffineMaps() {}
  using StmtListType = llvm::iplist<Statement>;
  void walk(StmtListType::iterator Start, StmtListType::iterator End);
  void visitOperationStmt(OperationStmt *stmt);
  PassResult runOnMLFunction(MLFunction *f);
  using StmtWalker<ComposeAffineMaps>::walk;
};

} // end anonymous namespace

MLFunctionPass *mlir::createComposeAffineMapsPass() {
  return new ComposeAffineMaps();
}

void ComposeAffineMaps::walk(StmtListType::iterator Start,
                             StmtListType::iterator End) {
  while (Start != End) {
    walk(&(*Start));
    // Increment iterator after walk as visit function can mutate stmt list
    // ahead of 'Start'.
    ++Start;
  }
}

void ComposeAffineMaps::visitOperationStmt(OperationStmt *opStmt) {
  if (auto affineApplyOp = opStmt->dyn_cast<AffineApplyOp>()) {
    forwardSubstitute(affineApplyOp);
    bool allUsesEmpty = true;
    for (auto *result : affineApplyOp->getOperation()->getResults()) {
      if (!result->use_empty()) {
        allUsesEmpty = false;
        break;
      }
    }
    if (allUsesEmpty) {
      affineApplyOpsToErase.push_back(opStmt);
    }
  }
}

PassResult ComposeAffineMaps::runOnMLFunction(MLFunction *f) {
  affineApplyOpsToErase.clear();
  walk(f);
  for (auto *opStmt : affineApplyOpsToErase) {
    opStmt->erase();
  }
  return success();
}
