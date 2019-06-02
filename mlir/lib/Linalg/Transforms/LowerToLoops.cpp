//===- LowerToLoops.cpp - conversion from Linalg library ops to loops------===//
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

#include "mlir/EDSC/Helpers.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/Linalg/IR/LinalgOps.h"
#include "mlir/Linalg/IR/LinalgTypes.h"
#include "mlir/Linalg/Passes.h"
#include "mlir/Linalg/Utils/Utils.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Support/STLExtras.h"

using namespace mlir;
using namespace mlir::edsc;
using namespace mlir::edsc::intrinsics;
using namespace mlir::linalg;

// Creates a number of ranges equal to the number of results in `map`.
// The returned ranges correspond to the loop ranges, in the proper order, for
// which new loops will be created.
static SmallVector<Value *, 4> emitLoopRanges(FuncBuilder *b, Location loc,
                                              AffineMap map,
                                              ArrayRef<Value *> allViewSizes,
                                              FunctionConstants &state) {
  // Apply `map` to get view sizes in loop order.
  auto sizes = applyMapToValues(b, loc, map, allViewSizes, state);
  // Create a new range with the applied tile sizes.
  SmallVector<Value *, 4> res;
  for (unsigned idx = 0, e = map.getNumResults(); idx < e; ++idx) {
    res.push_back(b->create<RangeOp>(loc, state.getOrCreateIndex(0), sizes[idx],
                                     state.getOrCreateIndex(1)));
  }
  return res;
}

// Returns the linearized list of all view dimensions in a linalgOp. Appliying
// the inverse, concatenated loopToOperandRangeMaps to this list allows the
// derivation of loop ranges for any linalgOp.
static SmallVector<Value *, 8> getViewSizes(LinalgOp &linalgOp) {
  SmallVector<Value *, 8> res;
  using dim = ValueBuilder<linalg::DimOp>;
  for (auto v : linalgOp.getInputsAndOutputs()) {
    ViewType t = v->getType().cast<ViewType>();
    for (unsigned i = 0; i < t.getRank(); ++i)
      res.push_back(dim(v, i));
  }
  return res;
}

static void emitLinalgOpAsLoops(LinalgOp &linalgOp, FunctionConstants &state) {
  FuncBuilder b(linalgOp.getOperation());
  ScopedContext scope(b, linalgOp.getOperation()->getLoc());
  auto loopRanges = emitLoopRanges(
      scope.getBuilder(), scope.getLocation(),
      // The flattened loopToOperandRangesMaps is expected to be an invertible
      // permutation map (which is asserted in the inverse calculation).
      inversePermutation(concatAffineMaps(loopToOperandRangesMaps(linalgOp))),
      getViewSizes(linalgOp), state);

  SmallVector<IndexHandle, 4> parallelIvs(linalgOp.getNumParallelLoops());
  SmallVector<IndexHandle, 4> reductionIvs(linalgOp.getNumReductionLoops());
  auto pivs = IndexHandle::makeIndexHandlePointers(parallelIvs);
  auto rivs = IndexHandle::makeIndexHandlePointers(reductionIvs);
  assert(loopRanges.size() == pivs.size() + rivs.size());

  // clang-format off
  ArrayRef<Value *> ranges(loopRanges);
  LoopNestRangeBuilder(pivs, ranges.take_front(pivs.size()))([&] {
    LoopNestRangeBuilder(rivs, ranges.take_back(rivs.size()))(
        [&linalgOp, &parallelIvs, &reductionIvs] {
        SmallVector<mlir::Value *, 4> parallel(
            parallelIvs.begin(), parallelIvs.end());
        SmallVector<mlir::Value *, 4> reduction(
            reductionIvs.begin(), reductionIvs.end());
        mlir::linalg::emitScalarImplementation(parallel, reduction, linalgOp);
      });
    });
  // clang-format on
}

namespace {
struct LowerLinalgToLoopsPass : public FunctionPass<LowerLinalgToLoopsPass> {
  void runOnFunction();
};
} // namespace

void LowerLinalgToLoopsPass::runOnFunction() {
  auto &f = getFunction();
  FunctionConstants state(f);
  f.walk<LinalgOp>([&state](LinalgOp linalgOp) {
    emitLinalgOpAsLoops(linalgOp, state);
    linalgOp.getOperation()->erase();
  });
}

FunctionPassBase *mlir::linalg::createLowerLinalgToLoopsPass() {
  return new LowerLinalgToLoopsPass();
}

static PassRegistration<LowerLinalgToLoopsPass>
    pass("linalg-lower-to-loops",
         "Lower the operations from the linalg dialect into loops");
