//===- KernelOutlining.cpp - Implementation of GPU kernel outling ---------===//
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
// This file implements the GPU dialect kernel outlining pass.
//
//===----------------------------------------------------------------------===//

#include "mlir/GPU/GPUDialect.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"
#include "mlir/StandardOps/Ops.h"

using namespace mlir;

namespace {

template <typename OpTy>
void createForAllDimensions(OpBuilder &builder, Location loc,
                            SmallVectorImpl<Value *> &values) {
  for (StringRef dim : {"x", "y", "z"}) {
    Value *v = builder.create<OpTy>(loc, builder.getIndexType(),
                                    builder.getStringAttr(dim));
    values.push_back(v);
  }
}

// Add operations generating block/thread ids and gird/block dimensions at the
// beginning of `kernelFunc` and replace uses of the respective function args.
void injectGpuIndexOperations(Location loc, Function &kernelFunc) {
  OpBuilder OpBuilder(kernelFunc.getBody());
  SmallVector<Value *, 12> indexOps;
  createForAllDimensions<gpu::BlockId>(OpBuilder, loc, indexOps);
  createForAllDimensions<gpu::ThreadId>(OpBuilder, loc, indexOps);
  createForAllDimensions<gpu::GridDim>(OpBuilder, loc, indexOps);
  createForAllDimensions<gpu::BlockDim>(OpBuilder, loc, indexOps);
  // Replace the leading 12 function args with the respective thread/block index
  // operations. Iterate backwards since args are erased and indices change.
  for (int i = 11; i >= 0; --i) {
    auto &firstBlock = kernelFunc.front();
    firstBlock.getArgument(i)->replaceAllUsesWith(indexOps[i]);
    firstBlock.eraseArgument(i);
  }
}

// Outline the `gpu.launch` operation body into a kernel function.
Function *outlineKernelFunc(Module &module, gpu::LaunchOp &launchOp) {
  Location loc = launchOp.getLoc();
  SmallVector<Type, 4> kernelOperandTypes(launchOp.getKernelOperandTypes());
  FunctionType type =
      FunctionType::get(kernelOperandTypes, {}, module.getContext());
  std::string kernelFuncName =
      Twine(launchOp.getOperation()->getFunction()->getName(), "_kernel").str();
  Function *outlinedFunc = new mlir::Function(loc, kernelFuncName, type);
  outlinedFunc->getBody().takeBody(launchOp.getBody());
  Builder builder(&module);
  outlinedFunc->setAttr(gpu::GPUDialect::getKernelFuncAttrName(),
                        builder.getUnitAttr());
  injectGpuIndexOperations(loc, *outlinedFunc);
  module.getFunctions().push_back(outlinedFunc);
  return outlinedFunc;
}

// Replace `gpu.launch` operations with an `gpu.launch_func` operation launching
// `kernelFunc`.
void convertToLaunchFuncOp(gpu::LaunchOp &launchOp, Function &kernelFunc) {
  OpBuilder OpBuilder(launchOp);
  SmallVector<Value *, 4> kernelOperandValues(
      launchOp.getKernelOperandValues());
  OpBuilder.create<gpu::LaunchFuncOp>(
      launchOp.getLoc(), &kernelFunc, launchOp.getGridSizeOperandValues(),
      launchOp.getBlockSizeOperandValues(), kernelOperandValues);
  launchOp.erase();
}

} // namespace

class GpuKernelOutliningPass : public ModulePass<GpuKernelOutliningPass> {
public:
  void runOnModule() override {
    for (auto &func : getModule()) {
      func.walk<mlir::gpu::LaunchOp>([&](mlir::gpu::LaunchOp op) {
        Function *outlinedFunc = outlineKernelFunc(getModule(), op);
        convertToLaunchFuncOp(op, *outlinedFunc);
      });
    }
  }
};

ModulePassBase *createGpuKernelOutliningPass() {
  return new GpuKernelOutliningPass();
}

static PassRegistration<GpuKernelOutliningPass>
    pass("gpu-kernel-outlining",
         "Outline gpu.launch bodies to kernel functions.");
