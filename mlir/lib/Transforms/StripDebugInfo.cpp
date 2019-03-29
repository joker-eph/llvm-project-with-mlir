//===- StripDebugInfo.cpp - Pass to strip debug information ---------------===//
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

#include "mlir/IR/Function.h"
#include "mlir/IR/Instruction.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/Passes.h"

using namespace mlir;

namespace {
struct StripDebugInfo : public FunctionPass<StripDebugInfo> {
  PassResult runOnFunction() override;
};
} // end anonymous namespace

PassResult StripDebugInfo::runOnFunction() {
  Function &func = getFunction();
  UnknownLoc unknownLoc = UnknownLoc::get(func.getContext());

  // Strip the debug info from the function and its instructions.
  func.setLoc(unknownLoc);
  func.walk([&](Instruction *inst) { inst->setLoc(unknownLoc); });
  return success();
}

/// Creates a pass to strip debug information from a function.
FunctionPassBase *mlir::createStripDebugInfoPass() {
  return new StripDebugInfo();
}

static PassRegistration<StripDebugInfo>
    pass("strip-debuginfo", "Strip debug info from functions and instructions");
