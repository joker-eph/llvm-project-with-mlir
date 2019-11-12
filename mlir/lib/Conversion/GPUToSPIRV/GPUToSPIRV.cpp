//===- GPUToSPIRV.cpp - MLIR SPIR-V lowering passes -----------------------===//
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
// This file implements a pass to convert a kernel function in the GPU Dialect
// into a spv.module operation
//
//===----------------------------------------------------------------------===//
#include "mlir/Conversion/StandardToSPIRV/ConvertStandardToSPIRV.h"
#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Dialect/LoopOps/LoopOps.h"
#include "mlir/Dialect/SPIRV/SPIRVDialect.h"
#include "mlir/Dialect/SPIRV/SPIRVOps.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace {

/// Pattern to convert a loop::ForOp within kernel functions into spirv::LoopOp.
class ForOpConversion final : public SPIRVOpLowering<loop::ForOp> {
public:
  using SPIRVOpLowering<loop::ForOp>::SPIRVOpLowering;

  PatternMatchResult
  matchAndRewrite(Operation *op, ArrayRef<Value *> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

/// Pattern lowering GPU block/thread size/id to loading SPIR-V invocation
/// builin variables.
template <typename OpTy, spirv::BuiltIn builtin>
class LaunchConfigConversion : public SPIRVOpLowering<OpTy> {
public:
  using SPIRVOpLowering<OpTy>::SPIRVOpLowering;

  PatternMatchResult
  matchAndRewrite(Operation *op, ArrayRef<Value *> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

/// Pattern to convert a kernel function in GPU dialect (a FuncOp with the
/// attribute gpu.kernel) within a spv.module.
class KernelFnConversion final : public SPIRVOpLowering<FuncOp> {
public:
  using SPIRVOpLowering<FuncOp>::SPIRVOpLowering;

  PatternMatchResult
  matchAndRewrite(Operation *op, ArrayRef<Value *> operands,
                  ConversionPatternRewriter &rewriter) const override;
};

} // namespace

PatternMatchResult
ForOpConversion::matchAndRewrite(Operation *op, ArrayRef<Value *> operands,
                                 ConversionPatternRewriter &rewriter) const {
  // loop::ForOp can be lowered to the structured control flow represented by
  // spirv::LoopOp by making the continue block of the spirv::LoopOp the loop
  // latch and the merge block the exit block. The resulting spirv::LoopOp has a
  // single back edge from the continue to header block, and a single exit from
  // header to merge.
  auto forOp = cast<loop::ForOp>(op);
  loop::ForOpOperandAdaptor forOperands(operands);
  auto loc = op->getLoc();
  auto loopControl = rewriter.getI32IntegerAttr(
      static_cast<uint32_t>(spirv::LoopControl::None));
  auto loopOp = rewriter.create<spirv::LoopOp>(loc, loopControl);
  loopOp.addEntryAndMergeBlock();

  OpBuilder::InsertionGuard guard(rewriter);
  // Create the block for the header.
  auto header = new Block();
  // Insert the header.
  loopOp.body().getBlocks().insert(std::next(loopOp.body().begin(), 1), header);

  // Create the new induction variable to use.
  BlockArgument *newIndVar =
      header->addArgument(forOperands.lowerBound()->getType());
  Block *body = forOp.getBody();

  // Apply signature conversion to the body of the forOp. It has a single block,
  // with argument which is the induction variable. That has to be replaced with
  // the new induction variable.
  TypeConverter::SignatureConversion signatureConverter(
      body->getNumArguments());
  signatureConverter.remapInput(0, newIndVar);
  rewriter.applySignatureConversion(&forOp.getOperation()->getRegion(0),
                                    signatureConverter);

  // Delete the loop terminator.
  rewriter.eraseOp(body->getTerminator());

  // Move the blocks from the forOp into the loopOp. This is the body of the
  // loopOp.
  rewriter.inlineRegionBefore(forOp.getOperation()->getRegion(0), loopOp.body(),
                              std::next(loopOp.body().begin(), 2));

  // Branch into it from the entry.
  rewriter.setInsertionPointToEnd(&(loopOp.body().front()));
  rewriter.create<spirv::BranchOp>(loc, header, forOperands.lowerBound());

  // Generate the rest of the loop header.
  rewriter.setInsertionPointToEnd(header);
  auto mergeBlock = loopOp.getMergeBlock();
  auto cmpOp = rewriter.create<spirv::SLessThanOp>(
      loc, rewriter.getI1Type(), newIndVar, forOperands.upperBound());
  rewriter.create<spirv::BranchConditionalOp>(
      loc, cmpOp, body, ArrayRef<Value *>(), mergeBlock, ArrayRef<Value *>());

  // Generate instructions to increment the step of the induction variable and
  // branch to the header.
  Block *continueBlock = loopOp.getContinueBlock();
  rewriter.setInsertionPointToEnd(continueBlock);

  // Add the step to the induction variable and branch to the header.
  Value *updatedIndVar = rewriter.create<spirv::IAddOp>(
      loc, newIndVar->getType(), newIndVar, forOperands.step());
  rewriter.create<spirv::BranchOp>(loc, header, updatedIndVar);

  rewriter.eraseOp(forOp);
  return matchSuccess();
}

template <typename OpTy, spirv::BuiltIn builtin>
PatternMatchResult LaunchConfigConversion<OpTy, builtin>::matchAndRewrite(
    Operation *op, ArrayRef<Value *> operands,
    ConversionPatternRewriter &rewriter) const {
  auto dimAttr = op->getAttrOfType<StringAttr>("dimension");
  if (!dimAttr) {
    return this->matchFailure();
  }
  int32_t index = 0;
  if (dimAttr.getValue() == "x") {
    index = 0;
  } else if (dimAttr.getValue() == "y") {
    index = 1;
  } else if (dimAttr.getValue() == "z") {
    index = 2;
  } else {
    return this->matchFailure();
  }

  // SPIR-V invocation builtin variables are a vector of type <3xi32>
  auto spirvBuiltin = this->loadFromBuiltinVariable(op, builtin, rewriter);
  rewriter.replaceOpWithNewOp<spirv::CompositeExtractOp>(
      op, rewriter.getIntegerType(32), spirvBuiltin,
      rewriter.getI32ArrayAttr({index}));
  return this->matchSuccess();
}

PatternMatchResult
KernelFnConversion::matchAndRewrite(Operation *op, ArrayRef<Value *> operands,
                                    ConversionPatternRewriter &rewriter) const {
  auto funcOp = cast<FuncOp>(op);
  FuncOp newFuncOp;
  if (!gpu::GPUDialect::isKernel(funcOp)) {
    return succeeded(lowerFunction(funcOp, &typeConverter, rewriter, newFuncOp))
               ? matchSuccess()
               : matchFailure();
  }

  if (failed(
          lowerAsEntryFunction(funcOp, &typeConverter, rewriter, newFuncOp))) {
    return matchFailure();
  }
  return matchSuccess();
}

namespace {
/// Pass to lower GPU Dialect to SPIR-V. The pass only converts those functions
/// that have the "gpu.kernel" attribute, i.e. those functions that are
/// referenced in gpu::LaunchKernelOp operations. For each such function
///
/// 1) Create a spirv::ModuleOp, and clone the function into spirv::ModuleOp
/// (the original function is still needed by the gpu::LaunchKernelOp, so cannot
/// replace it).
///
/// 2) Lower the body of the spirv::ModuleOp.
class GPUToSPIRVPass : public ModulePass<GPUToSPIRVPass> {
  void runOnModule() override;
};
} // namespace

void GPUToSPIRVPass::runOnModule() {
  auto context = &getContext();
  auto module = getModule();

  SmallVector<Operation *, 4> spirvModules;
  module.walk([&module, &spirvModules](FuncOp funcOp) {
    if (gpu::GPUDialect::isKernel(funcOp)) {
      OpBuilder builder(module.getBodyRegion());
      // Create a new spirv::ModuleOp for this function, and clone the
      // function into it.
      // TODO : Generalize this to account for different extensions,
      // capabilities, extended_instruction_sets, other addressing models
      // and memory models.
      auto spvModule = builder.create<spirv::ModuleOp>(
          funcOp.getLoc(),
          builder.getI32IntegerAttr(
              static_cast<int32_t>(spirv::AddressingModel::Logical)),
          builder.getI32IntegerAttr(
              static_cast<int32_t>(spirv::MemoryModel::GLSL450)),
          builder.getStrArrayAttr(
              spirv::stringifyCapability(spirv::Capability::Shader)),
          builder.getStrArrayAttr(spirv::stringifyExtension(
              spirv::Extension::SPV_KHR_storage_buffer_storage_class)));
      // Hardwire the capability to be Shader.
      OpBuilder moduleBuilder(spvModule.getOperation()->getRegion(0));
      moduleBuilder.clone(*funcOp.getOperation());
      spirvModules.push_back(spvModule);
    }
  });

  /// Dialect conversion to lower the functions with the spirv::ModuleOps.
  SPIRVBasicTypeConverter basicTypeConverter;
  SPIRVTypeConverter typeConverter(&basicTypeConverter);
  OwningRewritePatternList patterns;
  patterns.insert<
      ForOpConversion, KernelFnConversion,
      LaunchConfigConversion<gpu::BlockDimOp, spirv::BuiltIn::WorkgroupSize>,
      LaunchConfigConversion<gpu::BlockIdOp, spirv::BuiltIn::WorkgroupId>,
      LaunchConfigConversion<gpu::GridDimOp, spirv::BuiltIn::NumWorkgroups>,
      LaunchConfigConversion<gpu::ThreadIdOp,
                             spirv::BuiltIn::LocalInvocationId>>(context,
                                                                 typeConverter);
  populateStandardToSPIRVPatterns(context, patterns);

  ConversionTarget target(*context);
  target.addLegalDialect<spirv::SPIRVDialect>();
  target.addDynamicallyLegalOp<FuncOp>([&](FuncOp op) {
    // TODO(ravishankarm) : Currently lowering does not support handling
    // function conversion of non-kernel functions. This is to be added.

    // For kernel functions, verify that the signature is void(void).
    return gpu::GPUDialect::isKernel(op) && op.getNumResults() == 0 &&
           op.getNumArguments() == 0;
  });

  if (failed(applyFullConversion(spirvModules, target, patterns,
                                 &typeConverter))) {
    return signalPassFailure();
  }

  // After the SPIR-V modules have been generated, some finalization is needed
  // for the entry functions. For example, adding spv.EntryPoint op,
  // spv.ExecutionMode op, etc.
  for (auto *spvModule : spirvModules) {
    for (auto op :
         cast<spirv::ModuleOp>(spvModule).getBlock().getOps<FuncOp>()) {
      if (gpu::GPUDialect::isKernel(op)) {
        OpBuilder builder(op.getContext());
        builder.setInsertionPointAfter(op);
        if (failed(finalizeEntryFunction(op, builder))) {
          return signalPassFailure();
        }
        op.getOperation()->removeAttr(Identifier::get(
            gpu::GPUDialect::getKernelFuncAttrName(), op.getContext()));
      }
    }
  }
}

OpPassBase<ModuleOp> *createGPUToSPIRVPass() { return new GPUToSPIRVPass(); }

static PassRegistration<GPUToSPIRVPass>
    pass("convert-gpu-to-spirv", "Convert GPU dialect to SPIR-V dialect");
