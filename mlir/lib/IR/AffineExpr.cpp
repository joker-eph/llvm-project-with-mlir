//===- AffineExpr.cpp - MLIR Affine Expr Classes --------------------------===//
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

#include "mlir/IR/AffineExpr.h"
#include "AffineExprDetail.h"
#include "mlir/Support/STLExtras.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;
using namespace mlir::detail;

MLIRContext *AffineExpr::getContext() const {
  return expr->contextAndKind.getPointer();
}

AffineExprKind AffineExpr::getKind() const {
  return expr->contextAndKind.getInt();
}

/// Returns true if this expression is made out of only symbols and
/// constants (no dimensional identifiers).
bool AffineExpr::isSymbolicOrConstant() const {
  switch (getKind()) {
  case AffineExprKind::Constant:
    return true;
  case AffineExprKind::DimId:
    return false;
  case AffineExprKind::SymbolId:
    return true;

  case AffineExprKind::Add:
  case AffineExprKind::Mul:
  case AffineExprKind::FloorDiv:
  case AffineExprKind::CeilDiv:
  case AffineExprKind::Mod: {
    auto expr = this->cast<AffineBinaryOpExpr>();
    return expr.getLHS().isSymbolicOrConstant() &&
           expr.getRHS().isSymbolicOrConstant();
  }
  }
}

/// Returns true if this is a pure affine expression, i.e., multiplication,
/// floordiv, ceildiv, and mod is only allowed w.r.t constants.
bool AffineExpr::isPureAffine() const {
  switch (getKind()) {
  case AffineExprKind::SymbolId:
  case AffineExprKind::DimId:
  case AffineExprKind::Constant:
    return true;
  case AffineExprKind::Add: {
    auto op = cast<AffineBinaryOpExpr>();
    return op.getLHS().isPureAffine() && op.getRHS().isPureAffine();
  }

  case AffineExprKind::Mul: {
    // TODO: Canonicalize the constants in binary operators to the RHS when
    // possible, allowing this to merge into the next case.
    auto op = cast<AffineBinaryOpExpr>();
    return op.getLHS().isPureAffine() && op.getRHS().isPureAffine() &&
           (op.getLHS().template isa<AffineConstantExpr>() ||
            op.getRHS().template isa<AffineConstantExpr>());
  }
  case AffineExprKind::FloorDiv:
  case AffineExprKind::CeilDiv:
  case AffineExprKind::Mod: {
    auto op = cast<AffineBinaryOpExpr>();
    return op.getLHS().isPureAffine() &&
           op.getRHS().template isa<AffineConstantExpr>();
  }
  }
}

/// Returns the greatest known integral divisor of this affine expression.
uint64_t AffineExpr::getLargestKnownDivisor() const {
  AffineBinaryOpExpr binExpr(nullptr);
  switch (getKind()) {
  case AffineExprKind::SymbolId:
    LLVM_FALLTHROUGH;
  case AffineExprKind::DimId:
    return 1;
  case AffineExprKind::Constant:
    return std::abs(this->cast<AffineConstantExpr>().getValue());
  case AffineExprKind::Mul: {
    binExpr = this->cast<AffineBinaryOpExpr>();
    return binExpr.getLHS().getLargestKnownDivisor() *
           binExpr.getRHS().getLargestKnownDivisor();
  }
  case AffineExprKind::Add:
    LLVM_FALLTHROUGH;
  case AffineExprKind::FloorDiv:
  case AffineExprKind::CeilDiv:
  case AffineExprKind::Mod: {
    binExpr = cast<AffineBinaryOpExpr>();
    return llvm::GreatestCommonDivisor64(
        binExpr.getLHS().getLargestKnownDivisor(),
        binExpr.getRHS().getLargestKnownDivisor());
  }
  }
}

bool AffineExpr::isMultipleOf(int64_t factor) const {
  AffineBinaryOpExpr binExpr(nullptr);
  uint64_t l, u;
  switch (getKind()) {
  case AffineExprKind::SymbolId:
    LLVM_FALLTHROUGH;
  case AffineExprKind::DimId:
    return factor * factor == 1;
  case AffineExprKind::Constant:
    return cast<AffineConstantExpr>().getValue() % factor == 0;
  case AffineExprKind::Mul: {
    binExpr = cast<AffineBinaryOpExpr>();
    // It's probably not worth optimizing this further (to not traverse the
    // whole sub-tree under - it that would require a version of isMultipleOf
    // that on a 'false' return also returns the largest known divisor).
    return (l = binExpr.getLHS().getLargestKnownDivisor()) % factor == 0 ||
           (u = binExpr.getRHS().getLargestKnownDivisor()) % factor == 0 ||
           (l * u) % factor == 0;
  }
  case AffineExprKind::Add:
  case AffineExprKind::FloorDiv:
  case AffineExprKind::CeilDiv:
  case AffineExprKind::Mod: {
    binExpr = cast<AffineBinaryOpExpr>();
    return llvm::GreatestCommonDivisor64(
               binExpr.getLHS().getLargestKnownDivisor(),
               binExpr.getRHS().getLargestKnownDivisor()) %
               factor ==
           0;
  }
  }
}

bool AffineExpr::isFunctionOfDim(unsigned position) const {
  if (getKind() == AffineExprKind::DimId) {
    return *this == mlir::getAffineDimExpr(position, getContext());
  }
  if (auto expr = this->dyn_cast<AffineBinaryOpExpr>()) {
    return expr.getLHS().isFunctionOfDim(position) ||
           expr.getRHS().isFunctionOfDim(position);
  }
  return false;
}

AffineBinaryOpExpr::AffineBinaryOpExpr(AffineExpr::ImplType *ptr)
    : AffineExpr(ptr) {}
AffineExpr AffineBinaryOpExpr::getLHS() const {
  return static_cast<ImplType *>(expr)->lhs;
}
AffineExpr AffineBinaryOpExpr::getRHS() const {
  return static_cast<ImplType *>(expr)->rhs;
}

AffineDimExpr::AffineDimExpr(AffineExpr::ImplType *ptr) : AffineExpr(ptr) {}
unsigned AffineDimExpr::getPosition() const {
  return static_cast<ImplType *>(expr)->position;
}

AffineSymbolExpr::AffineSymbolExpr(AffineExpr::ImplType *ptr)
    : AffineExpr(ptr) {}
unsigned AffineSymbolExpr::getPosition() const {
  return static_cast<ImplType *>(expr)->position;
}

AffineConstantExpr::AffineConstantExpr(AffineExpr::ImplType *ptr)
    : AffineExpr(ptr) {}
int64_t AffineConstantExpr::getValue() const {
  return static_cast<ImplType *>(expr)->constant;
}

AffineExpr AffineExpr::operator+(int64_t v) const {
  return AffineBinaryOpExprStorage::get(AffineExprKind::Add, expr,
                                        getAffineConstantExpr(v, getContext()));
}
AffineExpr AffineExpr::operator+(AffineExpr other) const {
  return AffineBinaryOpExprStorage::get(AffineExprKind::Add, expr, other.expr);
}
AffineExpr AffineExpr::operator*(int64_t v) const {
  return AffineBinaryOpExprStorage::get(AffineExprKind::Mul, expr,
                                        getAffineConstantExpr(v, getContext()));
}
AffineExpr AffineExpr::operator*(AffineExpr other) const {
  return AffineBinaryOpExprStorage::get(AffineExprKind::Mul, expr, other.expr);
}
// Unary minus, delegate to operator*.
AffineExpr AffineExpr::operator-() const {
  return AffineBinaryOpExprStorage::get(
      AffineExprKind::Mul, expr, getAffineConstantExpr(-1, getContext()));
}
// Delegate to operator+.
AffineExpr AffineExpr::operator-(int64_t v) const { return *this + (-v); }
AffineExpr AffineExpr::operator-(AffineExpr other) const {
  return *this + (-other);
}
AffineExpr AffineExpr::floorDiv(uint64_t v) const {
  return AffineBinaryOpExprStorage::get(AffineExprKind::FloorDiv, expr,
                                        getAffineConstantExpr(v, getContext()));
}
AffineExpr AffineExpr::floorDiv(AffineExpr other) const {
  return AffineBinaryOpExprStorage::get(AffineExprKind::FloorDiv, expr,
                                        other.expr);
}
AffineExpr AffineExpr::ceilDiv(uint64_t v) const {
  return AffineBinaryOpExprStorage::get(AffineExprKind::CeilDiv, expr,
                                        getAffineConstantExpr(v, getContext()));
}
AffineExpr AffineExpr::ceilDiv(AffineExpr other) const {
  return AffineBinaryOpExprStorage::get(AffineExprKind::CeilDiv, expr,
                                        other.expr);
}
AffineExpr AffineExpr::operator%(uint64_t v) const {
  return AffineBinaryOpExprStorage::get(AffineExprKind::Mod, expr,
                                        getAffineConstantExpr(v, getContext()));
}
AffineExpr AffineExpr::operator%(AffineExpr other) const {
  return AffineBinaryOpExprStorage::get(AffineExprKind::Mod, expr, other.expr);
}

raw_ostream &operator<<(raw_ostream &os, AffineExpr &expr) {
  expr.print(os);
  return os;
}
