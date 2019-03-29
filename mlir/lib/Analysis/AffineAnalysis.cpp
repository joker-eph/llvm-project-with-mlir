//===- AffineAnalysis.cpp - Affine structures analysis routines -----------===//
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
// This file implements miscellaneous analysis routines for affine structures
// (expressions, maps, sets), and other utilities relying on such analysis.
//
//===----------------------------------------------------------------------===//

#include "mlir/Analysis/AffineAnalysis.h"
#include "mlir/Analysis/AffineStructures.h"
#include "mlir/Analysis/Utils.h"
#include "mlir/IR/AffineExprVisitor.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Statements.h"
#include "mlir/StandardOps/StandardOps.h"
#include "mlir/Support/Functional.h"
#include "mlir/Support/MathExtras.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;

/// Constructs an affine expression from a flat ArrayRef. If there are local
/// identifiers (neither dimensional nor symbolic) that appear in the sum of
/// products expression, 'localExprs' is expected to have the AffineExpr
/// for it, and is substituted into. The ArrayRef 'eq' is expected to be in the
/// format [dims, symbols, locals, constant term].
//  TODO(bondhugula): refactor getAddMulPureAffineExpr to reuse it from here.
static AffineExpr toAffineExpr(ArrayRef<int64_t> eq, unsigned numDims,
                               unsigned numSymbols,
                               ArrayRef<AffineExpr> localExprs,
                               MLIRContext *context) {
  // Assert expected numLocals = eq.size() - numDims - numSymbols - 1
  assert(eq.size() - numDims - numSymbols - 1 == localExprs.size() &&
         "unexpected number of local expressions");

  auto expr = getAffineConstantExpr(0, context);
  // Dimensions and symbols.
  for (unsigned j = 0; j < numDims + numSymbols; j++) {
    if (eq[j] == 0) {
      continue;
    }
    auto id = j < numDims ? getAffineDimExpr(j, context)
                          : getAffineSymbolExpr(j - numDims, context);
    expr = expr + id * eq[j];
  }

  // Local identifiers.
  for (unsigned j = numDims + numSymbols, e = eq.size() - 1; j < e; j++) {
    if (eq[j] == 0) {
      continue;
    }
    auto term = localExprs[j - numDims - numSymbols] * eq[j];
    expr = expr + term;
  }

  // Constant term.
  int64_t constTerm = eq[eq.size() - 1];
  if (constTerm != 0)
    expr = expr + constTerm;
  return expr;
}

namespace {

// This class is used to flatten a pure affine expression (AffineExpr,
// which is in a tree form) into a sum of products (w.r.t constants) when
// possible, and in that process simplifying the expression. The simplification
// performed includes the accumulation of contributions for each dimensional and
// symbolic identifier together, the simplification of floordiv/ceildiv/mod
// expressions and other simplifications that in turn happen as a result. A
// simplification that this flattening naturally performs is of simplifying the
// numerator and denominator of floordiv/ceildiv, and folding a modulo
// expression to a zero, if possible. Three examples are below:
//
// (d0 + 3 * d1) + d0) - 2 * d1) - d0 simplified to  d0 + d1
// (d0 - d0 mod 4 + 4) mod 4  simplified to 0.
// (3*d0 + 2*d1 + d0) floordiv 2 + d1 simplified to 2*d0 + 2*d1
//
// For a modulo, floordiv, or a ceildiv expression, an additional identifier
// (called a local identifier) is introduced to rewrite it as a sum of products
// (w.r.t constants). For example, for the second example above, d0 % 4 is
// replaced by d0 - 4*q with q being introduced: the expression then simplifies
// to: (d0 - (d0 - 4q) + 4) = 4q + 4, modulo of which w.r.t 4 simplifies to
// zero. Note that an affine expression may not always be expressible in a sum
// of products form involving just the original dimensional and symbolic
// identifiers, due to the presence of modulo/floordiv/ceildiv expressions
// that may not be eliminated after simplification; in such cases, the final
// expression can be reconstructed by replacing the local identifiers with their
// corresponding explicit form stored in 'localExprs' (note that the explicit
// form itself would have been simplified).
//
// This is a linear time post order walk for an affine expression that attempts
// the above simplifications through visit methods, with partial results being
// stored in 'operandExprStack'. When a parent expr is visited, the flattened
// expressions corresponding to its two operands would already be on the stack -
// the parent expression looks at the two flattened expressions and combines the
// two. It pops off the operand expressions and pushes the combined result
// (although this is done in-place on its LHS operand expr). When the walk is
// completed, the flattened form of the top-level expression would be left on
// the stack.
//
// A flattener can be repeatedly used for multiple affine expressions that bind
// to the same operands, for example, for all result expressions of an
// AffineMap or AffineValueMap. In such cases, using it for multiple expressions
// is more efficient than creating a new flattener for each expression since
// common idenical div and mod expressions appearing across different
// expressions are mapped to the local identifier (same column position in
// 'localVarCst').
struct AffineExprFlattener : public AffineExprVisitor<AffineExprFlattener> {
public:
  // Flattend expression layout: [dims, symbols, locals, constant]
  // Stack that holds the LHS and RHS operands while visiting a binary op expr.
  // In future, consider adding a prepass to determine how big the SmallVector's
  // will be, and linearize this to std::vector<int64_t> to prevent
  // SmallVector moves on re-allocation.
  std::vector<SmallVector<int64_t, 8>> operandExprStack;
  // Constraints connecting newly introduced local variables (for mod's and
  // div's) to existing (dimensional and symbolic) ones. These are always
  // inequalities.
  FlatAffineConstraints localVarCst;

  unsigned numDims;
  unsigned numSymbols;
  // Number of newly introduced identifiers to flatten mod/floordiv/ceildiv
  // expressions that could not be simplified.
  unsigned numLocals;
  // AffineExpr's corresponding to the floordiv/ceildiv/mod expressions for
  // which new identifiers were introduced; if the latter do not get canceled
  // out, these expressions are needed to reconstruct the AffineExpr / tree
  // form. Note that these expressions themselves would have been simplified
  // (recursively) by this pass. Eg. d0 + (d0 + 2*d1 + d0) ceildiv 4 will be
  // simplified to d0 + q, where q = (d0 + d1) ceildiv 2. (d0 + d1) ceildiv 2
  // would be the local expression stored for q.
  SmallVector<AffineExpr, 4> localExprs;
  MLIRContext *context;

  AffineExprFlattener(unsigned numDims, unsigned numSymbols,
                      MLIRContext *context)
      : numDims(numDims), numSymbols(numSymbols), numLocals(0),
        context(context) {
    operandExprStack.reserve(8);
    localVarCst.reset(numDims, numSymbols, numLocals);
  }

  void visitMulExpr(AffineBinaryOpExpr expr) {
    assert(operandExprStack.size() >= 2);
    // This is a pure affine expr; the RHS will be a constant.
    assert(expr.getRHS().isa<AffineConstantExpr>());
    // Get the RHS constant.
    auto rhsConst = operandExprStack.back()[getConstantIndex()];
    operandExprStack.pop_back();
    // Update the LHS in place instead of pop and push.
    auto &lhs = operandExprStack.back();
    for (unsigned i = 0, e = lhs.size(); i < e; i++) {
      lhs[i] *= rhsConst;
    }
  }

  void visitAddExpr(AffineBinaryOpExpr expr) {
    assert(operandExprStack.size() >= 2);
    const auto &rhs = operandExprStack.back();
    auto &lhs = operandExprStack[operandExprStack.size() - 2];
    assert(lhs.size() == rhs.size());
    // Update the LHS in place.
    for (unsigned i = 0, e = rhs.size(); i < e; i++) {
      lhs[i] += rhs[i];
    }
    // Pop off the RHS.
    operandExprStack.pop_back();
  }

  void visitModExpr(AffineBinaryOpExpr expr) {
    assert(operandExprStack.size() >= 2);
    // This is a pure affine expr; the RHS will be a constant.
    assert(expr.getRHS().isa<AffineConstantExpr>());
    auto rhsConst = operandExprStack.back()[getConstantIndex()];
    operandExprStack.pop_back();
    auto &lhs = operandExprStack.back();
    // TODO(bondhugula): handle modulo by zero case when this issue is fixed
    // at the other places in the IR.
    assert(rhsConst != 0 && "RHS constant can't be zero");

    // Check if the LHS expression is a multiple of modulo factor.
    unsigned i, e;
    for (i = 0, e = lhs.size(); i < e; i++)
      if (lhs[i] % rhsConst != 0)
        break;
    // If yes, modulo expression here simplifies to zero.
    if (i == lhs.size()) {
      std::fill(lhs.begin(), lhs.end(), 0);
      return;
    }

    // Add an existential quantifier. expr1 % c is replaced by (expr1 -
    // q * c) where q is the existential quantifier introduced.
    auto a = toAffineExpr(lhs, numDims, numSymbols, localExprs, context);
    auto b = getAffineConstantExpr(rhsConst, context);
    int loc;
    auto floorDiv = a.floorDiv(b);
    if ((loc = findLocalId(floorDiv)) == -1) {
      addLocalId(floorDiv);
      lhs[getLocalVarStartIndex() + numLocals - 1] = -rhsConst;
      // Update localVarCst:  0 <= expr1 - c * expr2  <= c - 1.
      localVarCst.addConstantLowerBound(lhs, 0);
      localVarCst.addConstantUpperBound(lhs, rhsConst - 1);
    } else {
      // Reuse the existing local id.
      lhs[getLocalVarStartIndex() + loc] = -rhsConst;
    }
  }
  void visitCeilDivExpr(AffineBinaryOpExpr expr) {
    visitDivExpr(expr, /*isCeil=*/true);
  }
  void visitFloorDivExpr(AffineBinaryOpExpr expr) {
    visitDivExpr(expr, /*isCeil=*/false);
  }
  void visitDimExpr(AffineDimExpr expr) {
    operandExprStack.emplace_back(SmallVector<int64_t, 32>(getNumCols(), 0));
    auto &eq = operandExprStack.back();
    assert(expr.getPosition() < numDims && "Inconsistent number of dims");
    eq[getDimStartIndex() + expr.getPosition()] = 1;
  }
  void visitSymbolExpr(AffineSymbolExpr expr) {
    operandExprStack.emplace_back(SmallVector<int64_t, 32>(getNumCols(), 0));
    auto &eq = operandExprStack.back();
    assert(expr.getPosition() < numSymbols && "inconsistent number of symbols");
    eq[getSymbolStartIndex() + expr.getPosition()] = 1;
  }
  void visitConstantExpr(AffineConstantExpr expr) {
    operandExprStack.emplace_back(SmallVector<int64_t, 32>(getNumCols(), 0));
    auto &eq = operandExprStack.back();
    eq[getConstantIndex()] = expr.getValue();
  }

  // Simplify the affine expression by flattening it and reconstructing it.
  AffineExpr simplifyAffineExpr(AffineExpr expr) {
    // TODO(bondhugula): only pure affine for now. The simplification here can
    // be extended to semi-affine maps in the future.
    if (!expr.isPureAffine())
      return expr;

    walkPostOrder(expr);
    ArrayRef<int64_t> flattenedExpr = operandExprStack.back();
    auto simplifiedExpr = toAffineExpr(flattenedExpr, numDims, numSymbols,
                                       localExprs, expr.getContext());
    operandExprStack.pop_back();
    assert(operandExprStack.empty());
    return simplifiedExpr;
  }

private:
  void visitDivExpr(AffineBinaryOpExpr expr, bool isCeil) {
    assert(operandExprStack.size() >= 2);
    assert(expr.getRHS().isa<AffineConstantExpr>());
    // This is a pure affine expr; the RHS is a positive constant.
    auto rhsConst = operandExprStack.back()[getConstantIndex()];
    // TODO(bondhugula): handle division by zero at the same time the issue is
    // fixed at other places.
    assert(rhsConst != 0 && "RHS constant can't be zero");
    operandExprStack.pop_back();
    auto &lhs = operandExprStack.back();

    // Simplify the floordiv, ceildiv if possible by canceling out the greatest
    // common divisors of the numerator and denominator.
    uint64_t gcd = std::abs(rhsConst);
    for (unsigned i = 0, e = lhs.size(); i < e; i++)
      gcd = llvm::GreatestCommonDivisor64(gcd, std::abs(lhs[i]));
    // Simplify the numerator and the denominator.
    if (gcd != 1) {
      for (unsigned i = 0, e = lhs.size(); i < e; i++)
        lhs[i] = lhs[i] / static_cast<int64_t>(gcd);
    }
    int64_t denominator = rhsConst / gcd;
    // If the denominator becomes 1, the updated LHS is the result. (The
    // denominator can't be negative since rhsConst is positive).
    if (denominator == 1)
      return;

    // If the denominator cannot be simplified to one, we will have to retain
    // the ceil/floor expr (simplified up until here). Add an existential
    // quantifier to express its result, i.e., expr1 div expr2 is replaced
    // by a new identifier, q.
    auto a = toAffineExpr(lhs, numDims, numSymbols, localExprs, context);
    auto b = getAffineConstantExpr(denominator, context);

    int loc;
    auto div = isCeil ? a.ceilDiv(b) : a.floorDiv(b);
    if ((loc = findLocalId(div)) == -1) {
      addLocalId(div);
      std::vector<int64_t> bound(lhs.size(), 0);
      bound[getLocalVarStartIndex() + numLocals - 1] = rhsConst;
      if (!isCeil) {
        // q = lhs floordiv c  <=>  c*q <= lhs <= c*q + c - 1.
        localVarCst.addLowerBound(lhs, bound);
        bound[bound.size() - 1] = rhsConst - 1;
        localVarCst.addUpperBound(lhs, bound);
      } else {
        // q = lhs ceildiv c  <=>  c*q - (c - 1) <= lhs <= c*q.
        localVarCst.addUpperBound(lhs, bound);
        bound[bound.size() - 1] = -(rhsConst - 1);
        localVarCst.addLowerBound(lhs, bound);
      }
    }
    // Set the expression on stack to the local var introduced to capture the
    // result of the division (floor or ceil).
    std::fill(lhs.begin(), lhs.end(), 0);
    if (loc == -1)
      lhs[getLocalVarStartIndex() + numLocals - 1] = 1;
    else
      lhs[getLocalVarStartIndex() + loc] = 1;
  }

  // Add an existential quantifier (used to flatten a mod, floordiv, ceildiv
  // expr). localExpr is the simplified tree expression (AffineExpr)
  // corresponding to the quantifier.
  void addLocalId(AffineExpr localExpr) {
    for (auto &subExpr : operandExprStack) {
      subExpr.insert(subExpr.begin() + getLocalVarStartIndex() + numLocals, 0);
    }
    localExprs.push_back(localExpr);
    numLocals++;
    localVarCst.addLocalId(localVarCst.getNumLocalIds());
  }

  int findLocalId(AffineExpr localExpr) {
    SmallVectorImpl<AffineExpr>::iterator it;
    if ((it = std::find(localExprs.begin(), localExprs.end(), localExpr)) ==
        localExprs.end())
      return -1;
    return it - localExprs.begin();
  }

  inline unsigned getNumCols() const {
    return numDims + numSymbols + numLocals + 1;
  }
  inline unsigned getConstantIndex() const { return getNumCols() - 1; }
  inline unsigned getLocalVarStartIndex() const { return numDims + numSymbols; }
  inline unsigned getSymbolStartIndex() const { return numDims; }
  inline unsigned getDimStartIndex() const { return 0; }
};

} // end anonymous namespace

AffineExpr mlir::simplifyAffineExpr(AffineExpr expr, unsigned numDims,
                                    unsigned numSymbols) {
  AffineExprFlattener flattener(numDims, numSymbols, expr.getContext());
  return flattener.simplifyAffineExpr(expr);
}

/// Returns the AffineExpr that results from substituting `exprs[i]` into `e`
/// for each AffineDimExpr of position i in `e`.
/// Precondition: the maximal AffineDimExpr position in `e` is smaller than
/// `exprs.size()`.
static AffineExpr substExprs(AffineExpr e, llvm::ArrayRef<AffineExpr> exprs) {
  if (auto binExpr = e.dyn_cast<AffineBinaryOpExpr>()) {
    AffineExpr lhs, rhs;
    AffineExprBinaryOp binOp;
    std::tie(lhs, rhs, binOp) = matchBinaryOpExpr(binExpr);
    return binOp(substExprs(lhs, exprs), substExprs(rhs, exprs));
  }
  if (auto dim = e.dyn_cast<AffineDimExpr>()) {
    assert(dim.getPosition() < exprs.size() &&
           "Cannot compose due to dim mismatch");
    return exprs[dim.getPosition()];
  }
  if (auto sym = e.dyn_cast<AffineSymbolExpr>()) {
    return sym;
  }
  return e.template cast<AffineConstantExpr>();
}

AffineMap mlir::composeUnboundedMaps(AffineMap f, AffineMap g) {
  assert(f.getNumDims() == g.getNumResults() &&
         "Num dims of f must be the same as num results of g for maps to be "
         "composable");
  assert(g.getRangeSizes().empty() && "Expected unbounded AffineMap");
  assert(f.getRangeSizes().empty() && "Expected unbounded AffineMap");
  auto exprs = functional::map(
      [g](AffineExpr expr) { return mlir::composeWithUnboundedMap(expr, g); },
      f.getResults());
  auto composed =
      AffineMap::get(g.getNumDims(),
                     std::max(f.getNumSymbols(), g.getNumSymbols()), exprs, {});
  return composed;
}

AffineExpr mlir::composeWithUnboundedMap(AffineExpr e, AffineMap g) {
  return simplifyAffineExpr(substExprs(e, g.getResults()), g.getNumDims(),
                            g.getNumSymbols());
}

// Flattens the expressions in map. Returns true on success or false
// if 'expr' was unable to be flattened (i.e., semi-affine expressions not
// handled yet).
static bool getFlattenedAffineExprs(
    ArrayRef<AffineExpr> exprs, unsigned numDims, unsigned numSymbols,
    std::vector<llvm::SmallVector<int64_t, 8>> *flattenedExprs,
    FlatAffineConstraints *localVarCst) {
  if (exprs.empty()) {
    localVarCst->reset(numDims, numSymbols);
    return true;
  }

  flattenedExprs->reserve(exprs.size());

  AffineExprFlattener flattener(numDims, numSymbols, exprs[0].getContext());
  // Use the same flattener to simplify each expression successively. This way
  // local identifiers / expressions are shared.
  for (auto expr : exprs) {
    if (!expr.isPureAffine())
      return false;

    flattener.walkPostOrder(expr);
  }

  flattenedExprs->insert(flattenedExprs->end(),
                         flattener.operandExprStack.begin(),
                         flattener.operandExprStack.end());
  if (localVarCst)
    localVarCst->clearAndCopyFrom(flattener.localVarCst);

  return true;
}

// Flattens 'expr' into 'flattenedExpr'. Returns true on success or false
// if 'expr' was unable to be flattened (semi-affine expressions not handled
// yet).
bool mlir::getFlattenedAffineExpr(AffineExpr expr, unsigned numDims,
                                  unsigned numSymbols,
                                  llvm::SmallVectorImpl<int64_t> *flattenedExpr,
                                  FlatAffineConstraints *localVarCst) {
  std::vector<SmallVector<int64_t, 8>> flattenedExprs;
  bool ret = ::getFlattenedAffineExprs({expr}, numDims, numSymbols,
                                       &flattenedExprs, localVarCst);
  *flattenedExpr = flattenedExprs[0];
  return ret;
}

/// Flattens the expressions in map. Returns true on success or false
/// if 'expr' was unable to be flattened (i.e., semi-affine expressions not
/// handled yet).
bool mlir::getFlattenedAffineExprs(
    AffineMap map, std::vector<llvm::SmallVector<int64_t, 8>> *flattenedExprs,
    FlatAffineConstraints *localVarCst) {
  if (map.getNumResults() == 0) {
    localVarCst->reset(map.getNumDims(), map.getNumSymbols());
    return true;
  }
  return ::getFlattenedAffineExprs(map.getResults(), map.getNumDims(),
                                   map.getNumSymbols(), flattenedExprs,
                                   localVarCst);
}

bool mlir::getFlattenedAffineExprs(
    IntegerSet set, std::vector<llvm::SmallVector<int64_t, 8>> *flattenedExprs,
    FlatAffineConstraints *localVarCst) {
  if (set.getNumConstraints() == 0) {
    localVarCst->reset(set.getNumDims(), set.getNumSymbols());
    return true;
  }
  return ::getFlattenedAffineExprs(set.getConstraints(), set.getNumDims(),
                                   set.getNumSymbols(), flattenedExprs,
                                   localVarCst);
}

/// Returns the sequence of AffineApplyOp OperationInsts operation in
/// 'affineApplyOps', which are reachable via a search starting from 'operands',
/// and ending at operands which are not defined by AffineApplyOps.
// TODO(andydavis) Add a method to AffineApplyOp which forward substitutes
// the AffineApplyOp into any user AffineApplyOps.
void mlir::getReachableAffineApplyOps(
    ArrayRef<Value *> operands,
    SmallVectorImpl<OperationInst *> &affineApplyOps) {
  struct State {
    // The ssa value for this node in the DFS traversal.
    Value *value;
    // The operand index of 'value' to explore next during DFS traversal.
    unsigned operandIndex;
  };
  SmallVector<State, 4> worklist;
  for (auto *operand : operands) {
    worklist.push_back({operand, 0});
  }

  while (!worklist.empty()) {
    State &state = worklist.back();
    auto *opStmt = state.value->getDefiningInst();
    // Note: getDefiningInst will return nullptr if the operand is not an
    // OperationInst (i.e. ForStmt), which is a terminator for the search.
    if (opStmt == nullptr || !opStmt->isa<AffineApplyOp>()) {
      worklist.pop_back();
      continue;
    }
    if (auto affineApplyOp = opStmt->dyn_cast<AffineApplyOp>()) {
      if (state.operandIndex == 0) {
        // Pre-Visit: Add 'opStmt' to reachable sequence.
        affineApplyOps.push_back(opStmt);
      }
      if (state.operandIndex < opStmt->getNumOperands()) {
        // Visit: Add next 'affineApplyOp' operand to worklist.
        // Get next operand to visit at 'operandIndex'.
        auto *nextOperand = opStmt->getOperand(state.operandIndex);
        // Increment 'operandIndex' in 'state'.
        ++state.operandIndex;
        // Add 'nextOperand' to worklist.
        worklist.push_back({nextOperand, 0});
      } else {
        // Post-visit: done visiting operands AffineApplyOp, pop off stack.
        worklist.pop_back();
      }
    }
  }
}

// Forward substitutes into 'valueMap' all AffineApplyOps reachable from the
// operands of 'valueMap'.
void mlir::forwardSubstituteReachableOps(AffineValueMap *valueMap) {
  // Gather AffineApplyOps reachable from 'indices'.
  SmallVector<OperationInst *, 4> affineApplyOps;
  getReachableAffineApplyOps(valueMap->getOperands(), affineApplyOps);
  // Compose AffineApplyOps in 'affineApplyOps'.
  for (auto *opStmt : affineApplyOps) {
    assert(opStmt->isa<AffineApplyOp>());
    auto affineApplyOp = opStmt->dyn_cast<AffineApplyOp>();
    // Forward substitute 'affineApplyOp' into 'valueMap'.
    valueMap->forwardSubstitute(*affineApplyOp);
  }
}

// Builds a system of constraints with dimensional identifiers corresponding to
// the loop IVs of the forStmts appearing in that order. Any symbols founds in
// the bound operands are added as symbols in the system. Returns false for the
// yet unimplemented cases.
// TODO(andydavis,bondhugula) Handle non-unit steps through local variables or
// stride information in FlatAffineConstraints. (For eg., by using iv - lb %
// step = 0 and/or by introducing a method in FlatAffineConstraints
// setExprStride(ArrayRef<int64_t> expr, int64_t stride)
bool mlir::getIndexSet(ArrayRef<ForStmt *> forStmts,
                       FlatAffineConstraints *domain) {
  SmallVector<Value *, 4> indices(forStmts.begin(), forStmts.end());
  // Reset while associated Values in 'indices' to the domain.
  domain->reset(forStmts.size(), /*numSymbols=*/0, /*numLocals=*/0, indices);
  for (auto *forStmt : forStmts) {
    // Add constraints from forStmt's bounds.
    if (!domain->addForStmtDomain(*forStmt))
      return false;
  }
  return true;
}

// Computes the iteration domain for 'opStmt' and populates 'indexSet', which
// encapsulates the constraints involving loops surrounding 'opStmt' and
// potentially involving any Function symbols. The dimensional identifiers in
// 'indexSet' correspond to the loops surounding 'stmt' from outermost to
// innermost.
// TODO(andydavis) Add support to handle IfStmts surrounding 'stmt'.
static bool getStmtIndexSet(const Statement *stmt,
                            FlatAffineConstraints *indexSet) {
  // TODO(andydavis) Extend this to gather enclosing IfStmts and consider
  // factoring it out into a utility function.
  SmallVector<ForStmt *, 4> loops;
  getLoopIVs(*stmt, &loops);
  return getIndexSet(loops, indexSet);
}

// ValuePositionMap manages the mapping from Values which represent dimension
// and symbol identifiers from 'src' and 'dst' access functions to positions
// in new space where some Values are kept separate (using addSrc/DstValue)
// and some Values are merged (addSymbolValue).
// Position lookups return the absolute position in the new space which
// has the following format:
//
//   [src-dim-identifiers] [dst-dim-identifiers] [symbol-identifers]
//
// Note: access function non-IV dimension identifiers (that have 'dimension'
// positions in the access function position space) are assigned as symbols
// in the output position space. Convienience access functions which lookup
// an Value in multiple maps are provided (i.e. getSrcDimOrSymPos) to handle
// the common case of resolving positions for all access function operands.
//
// TODO(andydavis) Generalize this: could take a template parameter for
// the number of maps (3 in the current case), and lookups could take indices
// of maps to check. So getSrcDimOrSymPos would be "getPos(value, {0, 2})".
class ValuePositionMap {
public:
  void addSrcValue(const Value *value) {
    if (addValueAt(value, &srcDimPosMap, numSrcDims))
      ++numSrcDims;
  }
  void addDstValue(const Value *value) {
    if (addValueAt(value, &dstDimPosMap, numDstDims))
      ++numDstDims;
  }
  void addSymbolValue(const Value *value) {
    if (addValueAt(value, &symbolPosMap, numSymbols))
      ++numSymbols;
  }
  unsigned getSrcDimOrSymPos(const Value *value) const {
    return getDimOrSymPos(value, srcDimPosMap, 0);
  }
  unsigned getDstDimOrSymPos(const Value *value) const {
    return getDimOrSymPos(value, dstDimPosMap, numSrcDims);
  }
  unsigned getSymPos(const Value *value) const {
    auto it = symbolPosMap.find(value);
    assert(it != symbolPosMap.end());
    return numSrcDims + numDstDims + it->second;
  }

  unsigned getNumSrcDims() const { return numSrcDims; }
  unsigned getNumDstDims() const { return numDstDims; }
  unsigned getNumDims() const { return numSrcDims + numDstDims; }
  unsigned getNumSymbols() const { return numSymbols; }

private:
  bool addValueAt(const Value *value, DenseMap<const Value *, unsigned> *posMap,
                  unsigned position) {
    auto it = posMap->find(value);
    if (it == posMap->end()) {
      (*posMap)[value] = position;
      return true;
    }
    return false;
  }
  unsigned getDimOrSymPos(const Value *value,
                          const DenseMap<const Value *, unsigned> &dimPosMap,
                          unsigned dimPosOffset) const {
    auto it = dimPosMap.find(value);
    if (it != dimPosMap.end()) {
      return dimPosOffset + it->second;
    }
    it = symbolPosMap.find(value);
    assert(it != symbolPosMap.end());
    return numSrcDims + numDstDims + it->second;
  }

  unsigned numSrcDims = 0;
  unsigned numDstDims = 0;
  unsigned numSymbols = 0;
  DenseMap<const Value *, unsigned> srcDimPosMap;
  DenseMap<const Value *, unsigned> dstDimPosMap;
  DenseMap<const Value *, unsigned> symbolPosMap;
};

// Builds a map from Value to identifier position in a new merged identifier
// list, which is the result of merging dim/symbol lists from src/dst
// iteration domains. The format of the new merged list is as follows:
//
//   [src-dim-identifiers, dst-dim-identifiers, symbol-identifiers]
//
// This method populates 'valuePosMap' with mappings from operand Values in
// 'srcAccessMap'/'dstAccessMap' (as well as those in 'srcDomain'/'dstDomain')
// to the position of these values in the merged list.
static void buildDimAndSymbolPositionMaps(
    const FlatAffineConstraints &srcDomain,
    const FlatAffineConstraints &dstDomain, const AffineValueMap &srcAccessMap,
    const AffineValueMap &dstAccessMap, ValuePositionMap *valuePosMap) {
  auto updateValuePosMap = [&](ArrayRef<Value *> values, bool isSrc) {
    for (unsigned i = 0, e = values.size(); i < e; ++i) {
      auto *value = values[i];
      if (!isa<ForStmt>(values[i]))
        valuePosMap->addSymbolValue(value);
      else if (isSrc)
        valuePosMap->addSrcValue(value);
      else
        valuePosMap->addDstValue(value);
    }
  };

  SmallVector<Value *, 4> srcValues, destValues;
  srcDomain.getIdValues(&srcValues);
  dstDomain.getIdValues(&destValues);

  // Update value position map with identifiers from src iteration domain.
  updateValuePosMap(srcValues, /*isSrc=*/true);
  // Update value position map with identifiers from dst iteration domain.
  updateValuePosMap(destValues, /*isSrc=*/false);
  // Update value position map with identifiers from src access function.
  updateValuePosMap(srcAccessMap.getOperands(), /*isSrc=*/true);
  // Update value position map with identifiers from dst access function.
  updateValuePosMap(dstAccessMap.getOperands(), /*isSrc=*/false);
}

// Adds iteration domain constraints from 'srcDomain' and 'dstDomain' into
// 'dependenceDomain'.
// Uses 'valuePosMap' to determine the position in 'dependenceDomain' to which a
// srcDomain/dstDomain Value maps.
static void addDomainConstraints(const FlatAffineConstraints &srcDomain,
                                 const FlatAffineConstraints &dstDomain,
                                 const ValuePositionMap &valuePosMap,
                                 FlatAffineConstraints *dependenceDomain) {
  unsigned srcNumIneq = srcDomain.getNumInequalities();
  unsigned srcNumDims = srcDomain.getNumDimIds();
  unsigned srcNumSymbols = srcDomain.getNumSymbolIds();
  unsigned srcNumIds = srcNumDims + srcNumSymbols;

  unsigned dstNumIneq = dstDomain.getNumInequalities();
  unsigned dstNumDims = dstDomain.getNumDimIds();
  unsigned dstNumSymbols = dstDomain.getNumSymbolIds();
  unsigned dstNumIds = dstNumDims + dstNumSymbols;

  SmallVector<int64_t, 4> ineq(dependenceDomain->getNumCols());
  // Add inequalities from src domain.
  for (unsigned i = 0; i < srcNumIneq; ++i) {
    // Zero fill.
    std::fill(ineq.begin(), ineq.end(), 0);
    // Set coefficients for identifiers corresponding to src domain.
    for (unsigned j = 0; j < srcNumIds; ++j)
      ineq[valuePosMap.getSrcDimOrSymPos(srcDomain.getIdValue(j))] =
          srcDomain.atIneq(i, j);
    // Set constant term.
    ineq[ineq.size() - 1] = srcDomain.atIneq(i, srcNumIds);
    // Add inequality constraint.
    dependenceDomain->addInequality(ineq);
  }
  // Add inequalities from dst domain.
  for (unsigned i = 0; i < dstNumIneq; ++i) {
    // Zero fill.
    std::fill(ineq.begin(), ineq.end(), 0);
    // Set coefficients for identifiers corresponding to dst domain.
    for (unsigned j = 0; j < dstNumIds; ++j)
      ineq[valuePosMap.getDstDimOrSymPos(dstDomain.getIdValue(j))] =
          dstDomain.atIneq(i, j);
    // Set constant term.
    ineq[ineq.size() - 1] = dstDomain.atIneq(i, dstNumIds);
    // Add inequality constraint.
    dependenceDomain->addInequality(ineq);
  }
}

// Adds equality constraints that equate src and dst access functions
// represented by 'srcAccessMap' and 'dstAccessMap' for each result.
// Requires that 'srcAccessMap' and 'dstAccessMap' have the same results count.
// For example, given the following two accesses functions to a 2D memref:
//
//   Source access function:
//     (a0 * d0 + a1 * s0 + a2, b0 * d0 + b1 * s0 + b2)
//
//   Destination acceses function:
//     (c0 * d0 + c1 * s0 + c2, f0 * d0 + f1 * s0 + f2)
//
// This method constructs the following equality constraints in
// 'dependenceDomain', by equating the access functions for each result
// (i.e. each memref dim). Notice that 'd0' for the destination access function
// is mapped into 'd0' in the equality constraint:
//
//   d0      d1      s0         c
//   --      --      --         --
//   a0     -c0      (a1 - c1)  (a1 - c2) = 0
//   b0     -f0      (b1 - f1)  (b1 - f2) = 0
//
// Returns false if any AffineExpr cannot be flattened (due to it being
// semi-affine). Returns true otherwise.
static bool
addMemRefAccessConstraints(const AffineValueMap &srcAccessMap,
                           const AffineValueMap &dstAccessMap,
                           const ValuePositionMap &valuePosMap,
                           FlatAffineConstraints *dependenceDomain) {
  AffineMap srcMap = srcAccessMap.getAffineMap();
  AffineMap dstMap = dstAccessMap.getAffineMap();
  assert(srcMap.getNumResults() == dstMap.getNumResults());
  unsigned numResults = srcMap.getNumResults();

  unsigned srcNumIds = srcMap.getNumDims() + srcMap.getNumSymbols();
  ArrayRef<Value *> srcOperands = srcAccessMap.getOperands();

  unsigned dstNumIds = dstMap.getNumDims() + dstMap.getNumSymbols();
  ArrayRef<Value *> dstOperands = dstAccessMap.getOperands();

  std::vector<SmallVector<int64_t, 8>> srcFlatExprs;
  std::vector<SmallVector<int64_t, 8>> destFlatExprs;
  FlatAffineConstraints srcLocalVarCst, destLocalVarCst;
  // Get flattened expressions for the source destination maps.
  if (!getFlattenedAffineExprs(srcMap, &srcFlatExprs, &srcLocalVarCst) ||
      !getFlattenedAffineExprs(dstMap, &destFlatExprs, &destLocalVarCst))
    return false;

  unsigned srcNumLocalIds = srcLocalVarCst.getNumLocalIds();
  unsigned dstNumLocalIds = destLocalVarCst.getNumLocalIds();
  unsigned numLocalIdsToAdd = srcNumLocalIds + dstNumLocalIds;
  for (unsigned i = 0; i < numLocalIdsToAdd; i++) {
    dependenceDomain->addLocalId(dependenceDomain->getNumLocalIds());
  }

  unsigned numDims = dependenceDomain->getNumDimIds();
  unsigned numSymbols = dependenceDomain->getNumSymbolIds();
  unsigned numSrcLocalIds = srcLocalVarCst.getNumLocalIds();

  // Equality to add.
  SmallVector<int64_t, 8> eq(dependenceDomain->getNumCols());
  for (unsigned i = 0; i < numResults; ++i) {
    // Zero fill.
    std::fill(eq.begin(), eq.end(), 0);

    // Flattened AffineExpr for src result 'i'.
    const auto &srcFlatExpr = srcFlatExprs[i];
    // Set identifier coefficients from src access function.
    for (unsigned j = 0, e = srcOperands.size(); j < e; ++j)
      eq[valuePosMap.getSrcDimOrSymPos(srcOperands[j])] = srcFlatExpr[j];
    // Local terms.
    for (unsigned j = 0, e = srcNumLocalIds; j < e; j++)
      eq[numDims + numSymbols + j] = srcFlatExpr[srcNumIds + j];
    // Set constant term.
    eq[eq.size() - 1] = srcFlatExpr[srcFlatExpr.size() - 1];

    // Flattened AffineExpr for dest result 'i'.
    const auto &destFlatExpr = destFlatExprs[i];
    // Set identifier coefficients from dst access function.
    for (unsigned j = 0, e = dstOperands.size(); j < e; ++j)
      eq[valuePosMap.getDstDimOrSymPos(dstOperands[j])] -= destFlatExpr[j];
    // Local terms.
    for (unsigned j = 0, e = dstNumLocalIds; j < e; j++)
      eq[numDims + numSymbols + numSrcLocalIds + j] =
          destFlatExpr[dstNumIds + j];
    // Set constant term.
    eq[eq.size() - 1] -= destFlatExpr[destFlatExpr.size() - 1];

    // Add equality constraint.
    dependenceDomain->addEquality(eq);
  }

  // Add equality constraints for any operands that are defined by constant ops.
  auto addEqForConstOperands = [&](ArrayRef<const Value *> operands) {
    for (unsigned i = 0, e = operands.size(); i < e; ++i) {
      if (isa<ForStmt>(operands[i]))
        continue;
      auto *symbol = operands[i];
      assert(symbol->isValidSymbol());
      // Check if the symbol is a constant.
      if (auto *opStmt = symbol->getDefiningInst()) {
        if (auto constOp = opStmt->dyn_cast<ConstantIndexOp>()) {
          dependenceDomain->setIdToConstant(valuePosMap.getSymPos(symbol),
                                            constOp->getValue());
        }
      }
    }
  };

  // Add equality constraints for any src symbols defined by constant ops.
  addEqForConstOperands(srcOperands);
  // Add equality constraints for any dst symbols defined by constant ops.
  addEqForConstOperands(dstOperands);

  // TODO(b/122081337): add srcLocalVarCst, destLocalVarCst to the dependence
  // domain.
  return true;
}

// Returns the number of outer loop common to 'src/dstDomain'.
static unsigned getNumCommonLoops(const FlatAffineConstraints &srcDomain,
                                  const FlatAffineConstraints &dstDomain) {
  // Find the number of common loops shared by src and dst accesses.
  unsigned minNumLoops =
      std::min(srcDomain.getNumDimIds(), dstDomain.getNumDimIds());
  unsigned numCommonLoops = 0;
  for (unsigned i = 0; i < minNumLoops; ++i) {
    if (!isa<ForStmt>(srcDomain.getIdValue(i)) ||
        !isa<ForStmt>(dstDomain.getIdValue(i)) ||
        srcDomain.getIdValue(i) != dstDomain.getIdValue(i))
      break;
    ++numCommonLoops;
  }
  return numCommonLoops;
}

// Returns StmtBlock common to 'srcAccess.opStmt' and 'dstAccess.opStmt'.
static StmtBlock *getCommonStmtBlock(const MemRefAccess &srcAccess,
                                     const MemRefAccess &dstAccess,
                                     const FlatAffineConstraints &srcDomain,
                                     unsigned numCommonLoops) {
  if (numCommonLoops == 0) {
    auto *block = srcAccess.opStmt->getBlock();
    while (block->getContainingStmt()) {
      block = block->getContainingStmt()->getBlock();
    }
    return block;
  }
  auto *commonForValue = srcDomain.getIdValue(numCommonLoops - 1);
  assert(isa<ForStmt>(commonForValue));
  return cast<ForStmt>(commonForValue)->getBody();
}

// Returns true if the ancestor operation statement of 'srcAccess' properly
// dominates the ancestor operation statement of 'dstAccess' in the same
// statement block. Returns false otherwise.
// Note that because 'srcAccess' or 'dstAccess' may be nested in conditionals,
// the function is named 'srcMayExecuteBeforeDst'.
// Note that 'numCommonLoops' is the number of contiguous surrounding outer
// loops.
static bool srcMayExecuteBeforeDst(const MemRefAccess &srcAccess,
                                   const MemRefAccess &dstAccess,
                                   const FlatAffineConstraints &srcDomain,
                                   unsigned numCommonLoops) {
  // Get StmtBlock common to 'srcAccess.opStmt' and 'dstAccess.opStmt'.
  auto *commonBlock =
      getCommonStmtBlock(srcAccess, dstAccess, srcDomain, numCommonLoops);
  // Check the dominance relationship between the respective ancestors of the
  // src and dst in the StmtBlock of the innermost among the common loops.
  auto *srcStmt = commonBlock->findAncestorStmtInBlock(*srcAccess.opStmt);
  assert(srcStmt != nullptr);
  auto *dstStmt = commonBlock->findAncestorStmtInBlock(*dstAccess.opStmt);
  assert(dstStmt != nullptr);
  return mlir::properlyDominates(*srcStmt, *dstStmt);
}

// Adds ordering constraints to 'dependenceDomain' based on number of loops
// common to 'src/dstDomain' and requested 'loopDepth'.
// Note that 'loopDepth' cannot exceed the number of common loops plus one.
// EX: Given a loop nest of depth 2 with IVs 'i' and 'j':
// *) If 'loopDepth == 1' then one constraint is added: i' >= i + 1
// *) If 'loopDepth == 2' then two constraints are added: i == i' and j' > j + 1
// *) If 'loopDepth == 3' then two constraints are added: i == i' and j == j'
static void addOrderingConstraints(const FlatAffineConstraints &srcDomain,
                                   const FlatAffineConstraints &dstDomain,
                                   unsigned loopDepth,
                                   FlatAffineConstraints *dependenceDomain) {
  unsigned numCols = dependenceDomain->getNumCols();
  SmallVector<int64_t, 4> eq(numCols);
  unsigned numSrcDims = srcDomain.getNumDimIds();
  unsigned numCommonLoops = getNumCommonLoops(srcDomain, dstDomain);
  unsigned numCommonLoopConstraints = std::min(numCommonLoops, loopDepth);
  for (unsigned i = 0; i < numCommonLoopConstraints; ++i) {
    std::fill(eq.begin(), eq.end(), 0);
    eq[i] = -1;
    eq[i + numSrcDims] = 1;
    if (i == loopDepth - 1) {
      eq[numCols - 1] = -1;
      dependenceDomain->addInequality(eq);
    } else {
      dependenceDomain->addEquality(eq);
    }
  }
}

// Returns true if 'isEq' constraint in 'dependenceDomain' has a single
// non-zero coefficient at (rowIdx, idPos). Returns false otherwise.
// TODO(andydavis) Move this function to FlatAffineConstraints.
static bool hasSingleNonZeroAt(unsigned idPos, unsigned rowIdx, bool isEq,
                               FlatAffineConstraints *dependenceDomain) {
  unsigned numCols = dependenceDomain->getNumCols();
  for (unsigned j = 0; j < numCols - 1; ++j) {
    int64_t v = isEq ? dependenceDomain->atEq(rowIdx, j)
                     : dependenceDomain->atIneq(rowIdx, j);
    if ((j == idPos && v == 0) || (j != idPos && v != 0))
      return false;
  }
  return true;
}

// Computes distance and direction vectors in 'dependences', by adding
// variables to 'dependenceDomain' which represent the difference of the IVs,
// eliminating all other variables, and reading off distance vectors from
// equality constraints (if possible), and direction vectors from inequalities.
static void computeDirectionVector(
    const FlatAffineConstraints &srcDomain,
    const FlatAffineConstraints &dstDomain, unsigned loopDepth,
    FlatAffineConstraints *dependenceDomain,
    llvm::SmallVector<DependenceComponent, 2> *dependenceComponents) {
  // Find the number of common loops shared by src and dst accesses.
  unsigned numCommonLoops = getNumCommonLoops(srcDomain, dstDomain);
  if (numCommonLoops == 0)
    return;
  // Compute direction vectors for requested loop depth.
  unsigned numIdsToEliminate = dependenceDomain->getNumIds();
  // Add new variables to 'dependenceDomain' to represent the direction
  // constraints for each shared loop.
  for (unsigned j = 0; j < numCommonLoops; ++j) {
    dependenceDomain->addDimId(j);
  }

  // Add equality contraints for each common loop, setting newly introduced
  // variable at column 'j' to the 'dst' IV minus the 'src IV.
  SmallVector<int64_t, 4> eq;
  eq.resize(dependenceDomain->getNumCols());
  for (unsigned j = 0; j < numCommonLoops; ++j) {
    std::fill(eq.begin(), eq.end(), 0);
    eq[j] = 1;
    eq[j + numCommonLoops] = 1;
    eq[j + 2 * numCommonLoops] = -1;
    dependenceDomain->addEquality(eq);
  }

  // Eliminate all variables other than the direction variables just added.
  dependenceDomain->projectOut(numCommonLoops, numIdsToEliminate);

  // Scan each common loop variable column and add direction vectors based
  // on eliminated constraint system.
  unsigned numCols = dependenceDomain->getNumCols();
  dependenceComponents->reserve(numCommonLoops);
  for (unsigned j = 0; j < numCommonLoops; ++j) {
    DependenceComponent depComp;
    for (unsigned i = 0, e = dependenceDomain->getNumEqualities(); i < e; ++i) {
      // Check for equality constraint with single non-zero in column 'j'.
      if (!hasSingleNonZeroAt(j, i, /*isEq=*/true, dependenceDomain))
        continue;
      // Get direction variable coefficient at (i, j).
      int64_t d = dependenceDomain->atEq(i, j);
      // Get constant coefficient at (i, numCols - 1).
      int64_t c = -dependenceDomain->atEq(i, numCols - 1);
      assert(c % d == 0 && "No dependence should have existed");
      depComp.lb = depComp.ub = c / d;
      dependenceComponents->push_back(depComp);
      break;
    }
    // Skip checking inequalities if we set 'depComp' based on equalities.
    if (depComp.lb.hasValue() || depComp.ub.hasValue())
      continue;
    // TODO(andydavis) Call FlatAffineConstraints::getConstantLower/UpperBound
    // Check inequalities to track direction range for each 'j'.
    for (unsigned i = 0, e = dependenceDomain->getNumInequalities(); i < e;
         ++i) {
      // Check for inequality constraint with single non-zero in column 'j'.
      if (!hasSingleNonZeroAt(j, i, /*isEq=*/false, dependenceDomain))
        continue;
      // Get direction variable coefficient at (i, j).
      int64_t d = dependenceDomain->atIneq(i, j);
      // Get constant coefficient at (i, numCols - 1).
      int64_t c = dependenceDomain->atIneq(i, numCols - 1);
      if (d < 0) {
        // Upper bound: add tightest upper bound.
        auto ub = mlir::floorDiv(c, -d);
        if (!depComp.ub.hasValue() || ub < depComp.ub.getValue())
          depComp.ub = ub;
      } else {
        // Lower bound: add tightest lower bound.
        auto lb = mlir::ceilDiv(-c, d);
        if (!depComp.lb.hasValue() || lb > depComp.lb.getValue())
          depComp.lb = lb;
      }
    }
    if (depComp.lb.hasValue() || depComp.ub.hasValue()) {
      if (depComp.lb.hasValue() && depComp.ub.hasValue())
        assert(depComp.lb.getValue() <= depComp.ub.getValue());
      dependenceComponents->push_back(depComp);
    }
  }
}

// Populates 'accessMap' with composition of AffineApplyOps reachable from
// indices of MemRefAccess.
void MemRefAccess::getAccessMap(AffineValueMap *accessMap) const {
  auto memrefType = memref->getType().cast<MemRefType>();
  // Create identity map with same number of dimensions as 'memrefType' rank.
  auto map = AffineMap::getMultiDimIdentityMap(memrefType.getRank(),
                                               memref->getType().getContext());
  // Reset 'accessMap' and 'map' and access 'indices'.
  accessMap->reset(map, indices);
  // Compose 'accessMap' with reachable AffineApplyOps.
  forwardSubstituteReachableOps(accessMap);
}

// Builds a flat affine constraint system to check if there exists a dependence
// between memref accesses 'srcAccess' and 'dstAccess'.
// Returns 'false' if the accesses can be definitively shown not to access the
// same element. Returns 'true' otherwise.
// If a dependence exists, returns in 'dependenceComponents' a direction
// vector for the dependence, with a component for each loop IV in loops
// common to both accesses (see Dependence in AffineAnalysis.h for details).
//
// The memref access dependence check is comprised of the following steps:
// *) Compute access functions for each access. Access functions are computed
//    using AffineValueMaps initialized with the indices from an access, then
//    composed with AffineApplyOps reachable from operands of that access,
//    until operands of the AffineValueMap are loop IVs or symbols.
// *) Build iteration domain constraints for each access. Iteration domain
//    constraints are pairs of inequality contraints representing the
//    upper/lower loop bounds for each ForStmt in the loop nest associated
//    with each access.
// *) Build dimension and symbol position maps for each access, which map
//    Values from access functions and iteration domains to their position
//    in the merged constraint system built by this method.
//
// This method builds a constraint system with the following column format:
//
//  [src-dim-identifiers, dst-dim-identifiers, symbols, constant]
//
// For example, given the following MLIR code with with "source" and
// "destination" accesses to the same memref labled, and symbols %M, %N, %K:
//
//   for %i0 = 0 to 100 {
//     for %i1 = 0 to 50 {
//       %a0 = affine_apply
//         (d0, d1) -> (d0 * 2 - d1 * 4 + s1, d1 * 3 - s0) (%i0, %i1)[%M, %N]
//       // Source memref access.
//       store %v0, %m[%a0#0, %a0#1] : memref<4x4xf32>
//     }
//   }
//
//   for %i2 = 0 to 100 {
//     for %i3 = 0 to 50 {
//       %a1 = affine_apply
//         (d0, d1) -> (d0 * 7 + d1 * 9 - s1, d1 * 11 + s0) (%i2, %i3)[%K, %M]
//       // Destination memref access.
//       %v1 = load %m[%a1#0, %a1#1] : memref<4x4xf32>
//     }
//   }
//
// The access functions would be the following:
//
//   src: (%i0 * 2 - %i1 * 4 + %N, %i1 * 3 - %M)
//   dst: (%i2 * 7 + %i3 * 9 - %M, %i3 * 11 - %K)
//
// The iteration domains for the src/dst accesses would be the following:
//
//   src: 0 <= %i0 <= 100, 0 <= %i1 <= 50
//   dst: 0 <= %i2 <= 100, 0 <= %i3 <= 50
//
// The symbols by both accesses would be assigned to a canonical position order
// which will be used in the dependence constraint system:
//
//   symbol name: %M  %N  %K
//   symbol  pos:  0   1   2
//
// Equality constraints are built by equating each result of src/destination
// access functions. For this example, the following two equality constraints
// will be added to the dependence constraint system:
//
//   [src_dim0, src_dim1, dst_dim0, dst_dim1, sym0, sym1, sym2, const]
//      2         -4        -7        -9       1      1     0     0    = 0
//      0          3         0        -11     -1      0     1     0    = 0
//
// Inequality constraints from the iteration domain will be meged into
// the dependence constraint system
//
//   [src_dim0, src_dim1, dst_dim0, dst_dim1, sym0, sym1, sym2, const]
//       1         0         0         0        0     0     0     0    >= 0
//      -1         0         0         0        0     0     0     100  >= 0
//       0         1         0         0        0     0     0     0    >= 0
//       0        -1         0         0        0     0     0     50   >= 0
//       0         0         1         0        0     0     0     0    >= 0
//       0         0        -1         0        0     0     0     100  >= 0
//       0         0         0         1        0     0     0     0    >= 0
//       0         0         0        -1        0     0     0     50   >= 0
//
//
// TODO(andydavis) Support AffineExprs mod/floordiv/ceildiv.
bool mlir::checkMemrefAccessDependence(
    const MemRefAccess &srcAccess, const MemRefAccess &dstAccess,
    unsigned loopDepth, FlatAffineConstraints *dependenceConstraints,
    llvm::SmallVector<DependenceComponent, 2> *dependenceComponents) {
  // Return 'false' if these accesses do not acces the same memref.
  if (srcAccess.memref != dstAccess.memref)
    return false;
  // Return 'false' if one of these accesses is not a StoreOp.
  if (!srcAccess.opStmt->isa<StoreOp>() && !dstAccess.opStmt->isa<StoreOp>())
    return false;

  // Get composed access function for 'srcAccess'.
  AffineValueMap srcAccessMap;
  srcAccess.getAccessMap(&srcAccessMap);

  // Get composed access function for 'dstAccess'.
  AffineValueMap dstAccessMap;
  dstAccess.getAccessMap(&dstAccessMap);

  // Get iteration domain for the 'srcAccess' statement.
  FlatAffineConstraints srcDomain;
  if (!getStmtIndexSet(srcAccess.opStmt, &srcDomain))
    return false;

  // Get iteration domain for 'dstAccess' statement.
  FlatAffineConstraints dstDomain;
  if (!getStmtIndexSet(dstAccess.opStmt, &dstDomain))
    return false;

  // Return 'false' if loopDepth > numCommonLoops and if the ancestor operation
  // statement of 'srcAccess' does not properly dominate the ancestor operation
  // statement of 'dstAccess' in the same common statement block.
  unsigned numCommonLoops = getNumCommonLoops(srcDomain, dstDomain);
  assert(loopDepth <= numCommonLoops + 1);
  if (loopDepth > numCommonLoops &&
      !srcMayExecuteBeforeDst(srcAccess, dstAccess, srcDomain,
                              numCommonLoops)) {
    return false;
  }
  // Build dim and symbol position maps for each access from access operand
  // Value to position in merged contstraint system.
  ValuePositionMap valuePosMap;
  buildDimAndSymbolPositionMaps(srcDomain, dstDomain, srcAccessMap,
                                dstAccessMap, &valuePosMap);
  assert(valuePosMap.getNumDims() ==
         srcDomain.getNumDimIds() + dstDomain.getNumDimIds());

  // Calculate number of equalities/inequalities and columns required to
  // initialize FlatAffineConstraints for 'dependenceDomain'.
  unsigned numIneq =
      srcDomain.getNumInequalities() + dstDomain.getNumInequalities();
  AffineMap srcMap = srcAccessMap.getAffineMap();
  assert(srcMap.getNumResults() == dstAccessMap.getAffineMap().getNumResults());
  unsigned numEq = srcMap.getNumResults();
  unsigned numDims = srcDomain.getNumDimIds() + dstDomain.getNumDimIds();
  unsigned numSymbols = valuePosMap.getNumSymbols();
  unsigned numIds = numDims + numSymbols;
  unsigned numCols = numIds + 1;

  // Create flat affine constraints reserving space for 'numEq' and 'numIneq'.
  dependenceConstraints->reset(numIneq, numEq, numCols, numDims, numSymbols,
                               /*numLocals=*/0);
  // Create memref access constraint by equating src/dst access functions.
  // Note that this check is conservative, and will failure in the future
  // when local variables for mod/div exprs are supported.
  if (!addMemRefAccessConstraints(srcAccessMap, dstAccessMap, valuePosMap,
                                  dependenceConstraints))
    return true;

  // Add 'src' happens before 'dst' ordering constraints.
  addOrderingConstraints(srcDomain, dstDomain, loopDepth,
                         dependenceConstraints);
  // Add src and dst domain constraints.
  addDomainConstraints(srcDomain, dstDomain, valuePosMap,
                       dependenceConstraints);

  // Return false if the solution space is empty: no dependence.
  if (dependenceConstraints->isEmpty()) {
    return false;
  }
  // Compute dependence direction vector and return true.
  if (dependenceComponents != nullptr) {
    computeDirectionVector(srcDomain, dstDomain, loopDepth,
                           dependenceConstraints, dependenceComponents);
  }
  return true;
}
