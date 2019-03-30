//===- UseDefAnalysis.cpp - Analysis for Transitive UseDef chains ---------===//
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
// This file implements Analysis functions specific to slicing in Function.
//
//===----------------------------------------------------------------------===//

#include "mlir/Analysis/SliceAnalysis.h"
#include "mlir/AffineOps/AffineOps.h"
#include "mlir/Analysis/VectorAnalysis.h"
#include "mlir/IR/Operation.h"
#include "mlir/Support/Functional.h"
#include "mlir/Support/STLExtras.h"

#include "llvm/ADT/SetVector.h"
#include <type_traits>

///
/// Implements Analysis functions specific to slicing in Function.
///

using namespace mlir;

using llvm::DenseSet;
using llvm::SetVector;

static void getForwardSliceImpl(Instruction *inst,
                                SetVector<Instruction *> *forwardSlice,
                                TransitiveFilter filter) {
  if (!inst) {
    return;
  }

  // Evaluate whether we should keep this use.
  // This is useful in particular to implement scoping; i.e. return the
  // transitive forwardSlice in the current scope.
  if (!filter(inst)) {
    return;
  }

  if (auto forOp = inst->dyn_cast<AffineForOp>()) {
    for (auto &u : forOp.getInductionVar()->getUses()) {
      auto *ownerInst = u.getOwner();
      if (forwardSlice->count(ownerInst) == 0) {
        getForwardSliceImpl(ownerInst, forwardSlice, filter);
      }
    }
  } else {
    assert(inst->getNumResults() <= 1 && "NYI: multiple results");
    if (inst->getNumResults() > 0) {
      for (auto &u : inst->getResult(0)->getUses()) {
        auto *ownerInst = u.getOwner();
        if (forwardSlice->count(ownerInst) == 0) {
          getForwardSliceImpl(ownerInst, forwardSlice, filter);
        }
      }
    }
  }

  forwardSlice->insert(inst);
}

void mlir::getForwardSlice(Instruction *inst,
                           SetVector<Instruction *> *forwardSlice,
                           TransitiveFilter filter) {
  getForwardSliceImpl(inst, forwardSlice, filter);
  // Don't insert the top level instruction, we just queried on it and don't
  // want it in the results.
  forwardSlice->remove(inst);

  // Reverse to get back the actual topological order.
  // std::reverse does not work out of the box on SetVector and I want an
  // in-place swap based thing (the real std::reverse, not the LLVM adapter).
  std::vector<Instruction *> v(forwardSlice->takeVector());
  forwardSlice->insert(v.rbegin(), v.rend());
}

static void getBackwardSliceImpl(Instruction *inst,
                                 SetVector<Instruction *> *backwardSlice,
                                 TransitiveFilter filter) {
  if (!inst) {
    return;
  }

  // Evaluate whether we should keep this def.
  // This is useful in particular to implement scoping; i.e. return the
  // transitive forwardSlice in the current scope.
  if (!filter(inst)) {
    return;
  }

  for (auto *operand : inst->getOperands()) {
    auto *inst = operand->getDefiningInst();
    if (backwardSlice->count(inst) == 0) {
      getBackwardSliceImpl(inst, backwardSlice, filter);
    }
  }

  backwardSlice->insert(inst);
}

void mlir::getBackwardSlice(Instruction *inst,
                            SetVector<Instruction *> *backwardSlice,
                            TransitiveFilter filter) {
  getBackwardSliceImpl(inst, backwardSlice, filter);

  // Don't insert the top level instruction, we just queried on it and don't
  // want it in the results.
  backwardSlice->remove(inst);
}

SetVector<Instruction *> mlir::getSlice(Instruction *inst,
                                        TransitiveFilter backwardFilter,
                                        TransitiveFilter forwardFilter) {
  SetVector<Instruction *> slice;
  slice.insert(inst);

  unsigned currentIndex = 0;
  SetVector<Instruction *> backwardSlice;
  SetVector<Instruction *> forwardSlice;
  while (currentIndex != slice.size()) {
    auto *currentInst = (slice)[currentIndex];
    // Compute and insert the backwardSlice starting from currentInst.
    backwardSlice.clear();
    getBackwardSlice(currentInst, &backwardSlice, backwardFilter);
    slice.insert(backwardSlice.begin(), backwardSlice.end());

    // Compute and insert the forwardSlice starting from currentInst.
    forwardSlice.clear();
    getForwardSlice(currentInst, &forwardSlice, forwardFilter);
    slice.insert(forwardSlice.begin(), forwardSlice.end());
    ++currentIndex;
  }
  return topologicalSort(slice);
}

namespace {
/// DFS post-order implementation that maintains a global count to work across
/// multiple invocations, to help implement topological sort on multi-root DAGs.
/// We traverse all instructions but only record the ones that appear in
/// `toSort` for the final result.
struct DFSState {
  DFSState(const SetVector<Instruction *> &set)
      : toSort(set), topologicalCounts(), seen() {}
  const SetVector<Instruction *> &toSort;
  SmallVector<Instruction *, 16> topologicalCounts;
  DenseSet<Instruction *> seen;
};
} // namespace

static void DFSPostorder(Instruction *current, DFSState *state) {
  assert(current->getNumResults() <= 1 && "NYI: multi-result");
  if (current->getNumResults() > 0) {
    for (auto &u : current->getResult(0)->getUses()) {
      auto *inst = u.getOwner();
      DFSPostorder(inst, state);
    }
  }
  bool inserted;
  using IterTy = decltype(state->seen.begin());
  IterTy iter;
  std::tie(iter, inserted) = state->seen.insert(current);
  if (inserted) {
    if (state->toSort.count(current) > 0) {
      state->topologicalCounts.push_back(current);
    }
  }
}

SetVector<Instruction *>
mlir::topologicalSort(const SetVector<Instruction *> &toSort) {
  if (toSort.empty()) {
    return toSort;
  }

  // Run from each root with global count and `seen` set.
  DFSState state(toSort);
  for (auto *s : toSort) {
    assert(toSort.count(s) == 1 && "NYI: multi-sets not supported");
    DFSPostorder(s, &state);
  }

  // Reorder and return.
  SetVector<Instruction *> res;
  for (auto it = state.topologicalCounts.rbegin(),
            eit = state.topologicalCounts.rend();
       it != eit; ++it) {
    res.insert(*it);
  }
  return res;
}
