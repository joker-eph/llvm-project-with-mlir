//===- Tiling.cpp - Implementation of linalg Tiling -----------------------===//
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
// This file implements the linalg dialect Tiling pass.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/Linalg/IR/LinalgTypes.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/Linalg/Utils/Intrinsics.h"
#include "mlir/Dialect/Linalg/Utils/Utils.h"
#include "mlir/Dialect/LoopOps/LoopOps.h"
#include "mlir/EDSC/Helpers.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineExprVisitor.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Support/STLExtras.h"
#include "mlir/Transforms/FoldUtils.h"

#include "llvm/Support/CommandLine.h"

using namespace mlir;
using namespace mlir::edsc;
using namespace mlir::edsc::intrinsics;
using namespace mlir::linalg;
using namespace mlir::linalg::intrinsics;
using namespace mlir::loop;

#define DEBUG_TYPE "linalg-tiling"

static llvm::cl::OptionCategory clOptionsCategory(DEBUG_TYPE " options");
static llvm::cl::list<unsigned>
    clTileSizes("linalg-tile-sizes",
                llvm::cl::desc("Tile sizes by which to tile linalg operations"),
                llvm::cl::ZeroOrMore, llvm::cl::MiscFlags::CommaSeparated,
                llvm::cl::cat(clOptionsCategory));

static bool isZero(Value *v) {
  return isa_and_nonnull<ConstantIndexOp>(v->getDefiningOp()) &&
         cast<ConstantIndexOp>(v->getDefiningOp()).getValue() == 0;
}

// Creates a number of ranges equal to the number of non-zero in `tileSizes`.
// One for each loop of the LinalgOp that is tiled. The `tileSizes` argument has
// one entry per surrounding loop. It uses zero as the convention that a
// particular loop is not tiled. This convention simplifies implementations by
// avoiding affine map manipulations.
// The returned ranges correspond to the loop ranges, in the proper order, that
// are tiled and for which new loops will be created.
static SmallVector<mlir::linalg::SubViewOp::Range, 4>
makeTiledLoopRanges(OpBuilder &b, Location loc, AffineMap map,
                    ArrayRef<Value *> allViewSizes,
                    ArrayRef<Value *> allTileSizes, OperationFolder *folder) {
  assert(allTileSizes.size() == map.getNumResults());
  // Apply `map` to get view sizes in loop order.
  auto viewSizes = applyMapToValues(b, loc, map, allViewSizes, folder);
  SmallVector<Value *, 4> tileSizes(allTileSizes.begin(), allTileSizes.end());

  // Traverse the tile sizes, which are in loop order, erase zeros everywhere.
  for (int idx = tileSizes.size() - 1; idx >= 0; --idx) {
    if (isZero(tileSizes[idx])) {
      viewSizes.erase(viewSizes.begin() + idx);
      tileSizes.erase(tileSizes.begin() + idx);
    }
  }

  // Create a new range with the applied tile sizes.
  SmallVector<mlir::linalg::SubViewOp::Range, 4> res;
  for (unsigned idx = 0, e = tileSizes.size(); idx < e; ++idx) {
    res.push_back(mlir::linalg::SubViewOp::Range{
        constant_index(folder, 0), viewSizes[idx], tileSizes[idx]});
  }
  return res;
}

namespace {
// Helper visitor to determine whether an AffineExpr is tiled.
// This is achieved by traversing every AffineDimExpr with position `pos` and
// checking whether the corresponding `tileSizes[pos]` is non-zero.
// This also enforces only positive coefficients occur in multiplications.
//
// Example:
//   `d0 + 2 * d1 + d3` is tiled by [0, 0, 0, 2] but not by [0, 0, 2, 0]
//
struct TileCheck : public AffineExprVisitor<TileCheck> {
  TileCheck(ArrayRef<Value *> tileSizes)
      : isTiled(false), tileSizes(tileSizes) {}

  void visitDimExpr(AffineDimExpr expr) {
    isTiled |= !isZero(tileSizes[expr.getPosition()]);
  }
  void visitAffineBinaryOpExpr(AffineBinaryOpExpr expr) {
    visit(expr.getLHS());
    visit(expr.getRHS());
    if (expr.getKind() == mlir::AffineExprKind::Mul)
      assert(expr.getRHS().cast<AffineConstantExpr>().getValue() > 0 &&
             "nonpositive multiplying coefficient");
  }
  bool isTiled;
  ArrayRef<Value *> tileSizes;
};
} // namespace

static bool isTiled(AffineExpr expr, ArrayRef<Value *> tileSizes) {
  if (!expr)
    return false;
  TileCheck t(tileSizes);
  t.visit(expr);
  return t.isTiled;
}

// Checks whether the view with index `viewIndex` within `linalgOp` varies with
// respect to a non-zero `tileSize`.
static bool isTiled(AffineMap map, ArrayRef<Value *> tileSizes) {
  if (!map)
    return false;
  for (unsigned r = 0; r < map.getNumResults(); ++r)
    if (isTiled(map.getResult(r), tileSizes))
      return true;
  return false;
}

static SmallVector<Value *, 4>
makeTiledViews(OpBuilder &b, Location loc, LinalgOp linalgOp,
               ArrayRef<Value *> ivs, ArrayRef<Value *> tileSizes,
               ArrayRef<Value *> viewSizes, OperationFolder *folder) {
  assert(ivs.size() == static_cast<size_t>(llvm::count_if(
                           llvm::make_range(tileSizes.begin(), tileSizes.end()),
                           [](Value *v) { return !isZero(v); })) &&
         "expected as many ivs as non-zero sizes");

  using edsc::intrinsics::select;
  using edsc::op::operator+;
  using edsc::op::operator<;

  // Construct (potentially temporary) mins and maxes on which to apply maps
  // that define tile subviews.
  SmallVector<Value *, 8> mins, maxes;
  for (unsigned idx = 0, idxIvs = 0, e = tileSizes.size(); idx < e; ++idx) {
    if (isZero(tileSizes[idx])) {
      mins.push_back(constant_index(folder, 0));
      maxes.push_back(viewSizes[idx]);
    } else {
      ValueHandle lb(ivs[idxIvs++]), step(tileSizes[idx]);
      mins.push_back(lb);
      maxes.push_back(lb + step);
    }
  }

  auto *op = linalgOp.getOperation();

  SmallVector<Value *, 4> res;
  res.reserve(op->getNumOperands());
  auto viewIteratorBegin = linalgOp.getInputsAndOutputs().begin();
  for (unsigned viewIndex = 0; viewIndex < linalgOp.getNumInputsAndOutputs();
       ++viewIndex) {
    Value *view = *(viewIteratorBegin + viewIndex);
    unsigned rank = view->getType().cast<MemRefType>().getRank();
    auto map = loopToOperandRangesMaps(linalgOp)[viewIndex];
    // If the view is not tiled, we can use it as is.
    if (!isTiled(map, tileSizes)) {
      res.push_back(view);
      continue;
    }

    // Construct a new subview for the tile.
    SmallVector<mlir::linalg::SubViewOp::Range, 4> subViewRangeOperands;
    subViewRangeOperands.reserve(rank * 3);
    for (unsigned r = 0; r < rank; ++r) {
      if (!isTiled(map.getSubMap({r}), tileSizes)) {
        subViewRangeOperands.push_back(mlir::linalg::SubViewOp::Range{
            constant_index(folder, 0), dim(view, r),
            constant_index(folder, 1)});
        continue;
      }

      auto m = map.getSubMap({r});
      auto *min = applyMapToValues(b, loc, m, mins, folder).front();
      auto *max = applyMapToValues(b, loc, m, maxes, folder).front();
      // Tiling creates a new slice at the proper index, the slice step is 1
      // (i.e. the slice view does not subsample, stepping occurs in the loop).
      subViewRangeOperands.push_back(
          mlir::linalg::SubViewOp::Range{min, max, constant_index(folder, 1)});
    }
    SmallVector<Value *, 12> subViewOperands;
    subViewOperands.reserve(subViewRangeOperands.size() * 3);
    for (auto r : subViewRangeOperands) {
      subViewOperands.push_back(r.min);
      subViewOperands.push_back(r.max);
      subViewOperands.push_back(r.step);
    }
    res.push_back(
        b.create<mlir::linalg::SubViewOp>(loc, view, subViewOperands));
  }

  // Traverse the mins/maxes and erase those that don't have uses left.
  // This is a special type of folding that we only apply when `folder` is
  // defined.
  if (folder) {
    mins.append(maxes.begin(), maxes.end());
    for (auto *v : mins)
      if (v->use_empty())
        v->getDefiningOp()->erase();
  }

  return res;
}

llvm::Optional<TiledLinalgOp>
mlir::linalg::tileLinalgOp(OpBuilder &b, LinalgOp op,
                           ArrayRef<Value *> tileSizes,
                           OperationFolder *folder) {
  // 1. Enforce the convention that "tiling by zero" skips tiling a particular
  // dimension. This convention is significantly simpler to handle instead of
  // adjusting affine maps to account for missing dimensions.
  assert(op.getNumParallelLoops() + op.getNumReductionLoops() +
                 op.getNumWindowLoops() ==
             tileSizes.size() &&
         "expected matching number of tile sizes and loops");
  OpBuilder::InsertionGuard g(b);
  b.setInsertionPoint(op);
  ScopedContext scope(b, op.getLoc());
  // 2. Build the tiled loop ranges.
  auto viewSizes = getViewSizes(op);
  // The flattened loopToOperandRangesMaps is expected to be an invertible
  // permutation map (asserted in the inverse calculation).
  auto viewSizesToLoopsMap =
      inversePermutation(concatAffineMaps(loopToOperandRangesMaps(op)));
  assert(viewSizesToLoopsMap && "expected invertible map");
  auto loopRanges =
      makeTiledLoopRanges(b, scope.getLocation(), viewSizesToLoopsMap,
                          viewSizes, tileSizes, folder);

  // 3. Create the tiled loops.
  LinalgOp res = op;
  SmallVector<IndexHandle, 4> ivs(loopRanges.size());
  auto pivs = makeIndexHandlePointers(ivs);
  LoopNestRangeBuilder(pivs, loopRanges)([&] {
    auto b = ScopedContext::getBuilder();
    auto loc = ScopedContext::getLocation();
    SmallVector<Value *, 4> ivValues(ivs.begin(), ivs.end());
    auto views =
        makeTiledViews(b, loc, op, ivValues, tileSizes, viewSizes, folder);
    auto operands = getAssumedNonViewOperands(op);
    views.append(operands.begin(), operands.end());
    res = op.clone(b, loc, views);
  });

  // 4. Gather the newly created loops and return them with the new op.
  SmallVector<ForOp, 8> loops;
  loops.reserve(ivs.size());
  for (auto iv : ivs)
    loops.push_back(loop::getForInductionVarOwner(iv));

  return TiledLinalgOp{res, loops};
}

llvm::Optional<TiledLinalgOp>
mlir::linalg::tileLinalgOp(OpBuilder &b, LinalgOp op,
                           ArrayRef<int64_t> tileSizes,
                           OperationFolder *folder) {
  if (tileSizes.empty())
    return llvm::None;

  // The following uses the convention that "tiling by zero" skips tiling a
  // particular dimension. This convention is significantly simpler to handle
  // instead of adjusting affine maps to account for missing dimensions.
  auto nLoops = op.getNumParallelLoops() + op.getNumReductionLoops() +
                op.getNumWindowLoops();
  tileSizes = tileSizes.take_front(nLoops);
  // If only 0 tilings are left, then return.
  if (llvm::all_of(tileSizes, [](int64_t v) { return v == 0; }))
    return llvm::None;

  // Create a builder for tile size constants.
  OpBuilder::InsertionGuard g(b);
  b.setInsertionPoint(op);
  ScopedContext scope(b, op.getLoc());

  // Materialize concrete tile size values to pass the generic tiling function.
  SmallVector<Value *, 8> tileSizeValues;
  tileSizeValues.reserve(tileSizes.size());
  for (auto ts : tileSizes)
    tileSizeValues.push_back(constant_index(folder, ts));
  // Pad tile sizes with zero values to enforce our convention.
  if (tileSizeValues.size() < nLoops) {
    for (unsigned i = tileSizeValues.size(); i < nLoops; ++i)
      tileSizeValues.push_back(constant_index(folder, 0));
  }

  return tileLinalgOp(b, op, tileSizeValues, folder);
}

static void tileLinalgOps(FuncOp f, ArrayRef<int64_t> tileSizes) {
  OpBuilder b(f);
  OperationFolder folder(f.getContext());
  f.walk([tileSizes, &b, &folder](LinalgOp op) {
    auto opLoopsPair = tileLinalgOp(b, op, tileSizes, &folder);
    // If tiling occurred successfully, erase old op.
    if (opLoopsPair)
      op.erase();
  });
  f.walk([](LinalgOp op) {
    if (!op.getOperation()->hasNoSideEffect())
      return;
    if (op.getOperation()->use_empty())
      op.erase();
  });
}

namespace {
struct LinalgTilingPass : public FunctionPass<LinalgTilingPass> {
  LinalgTilingPass() = default;
  LinalgTilingPass(ArrayRef<int64_t> sizes);

  void runOnFunction() override { tileLinalgOps(getFunction(), tileSizes); }

  SmallVector<int64_t, 8> tileSizes;
};
} // namespace

LinalgTilingPass::LinalgTilingPass(ArrayRef<int64_t> sizes) {
  this->tileSizes.assign(sizes.begin(), sizes.end());
}

std::unique_ptr<OpPassBase<FuncOp>>
mlir::linalg::createLinalgTilingPass(ArrayRef<int64_t> tileSizes) {
  return std::make_unique<LinalgTilingPass>(tileSizes);
}

static PassRegistration<LinalgTilingPass>
    pass("linalg-tile", "Tile operations in the linalg dialect", [] {
      auto pass = std::make_unique<LinalgTilingPass>();
      pass->tileSizes.assign(clTileSizes.begin(), clTileSizes.end());
      return pass;
    });
