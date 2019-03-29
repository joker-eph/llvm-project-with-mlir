//===- Types.h - MLIR EDSC Type System Implementation -----------*- C++ -*-===//
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

#include "mlir/EDSC/Types.h"
#include "mlir-c/Core.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/Support/STLExtras.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>

using llvm::errs;
using llvm::Twine;

using namespace mlir;
using namespace mlir::edsc;
using namespace mlir::edsc::detail;

namespace mlir {
namespace edsc {
namespace detail {

template <typename T> ArrayRef<T> copyIntoExprAllocator(ArrayRef<T> elements) {
  if (elements.empty()) {
    return {};
  }
  auto storage = Expr::globalAllocator()->Allocate<T>(elements.size());
  std::uninitialized_copy(elements.begin(), elements.end(), storage);
  return llvm::makeArrayRef(storage, elements.size());
}

struct ExprStorage {
  // Note: this structure is similar to OperationState, but stores lists in a
  // EDSC bump allocator.
  ExprKind kind;
  unsigned id;

  ArrayRef<Expr> operands;
  ArrayRef<Type> resultTypes;
  ArrayRef<NamedAttribute> attributes;

  ExprStorage(ExprKind kind, ArrayRef<Type> results, ArrayRef<Expr> children,
              ArrayRef<NamedAttribute> attrs, unsigned exprId = Expr::newId())
      : kind(kind), id(exprId) {
    operands = copyIntoExprAllocator(children);
    resultTypes = copyIntoExprAllocator(results);
    attributes = copyIntoExprAllocator(attrs);
  }
};

struct StmtStorage {
  StmtStorage(Bindable lhs, Expr rhs, llvm::ArrayRef<Stmt> enclosedStmts)
      : lhs(lhs), rhs(rhs), enclosedStmts(enclosedStmts) {}
  Bindable lhs;
  Expr rhs;
  ArrayRef<Stmt> enclosedStmts;
};

struct StmtBlockStorage {
  StmtBlockStorage(ArrayRef<Bindable> args, ArrayRef<Type> argTypes,
                   ArrayRef<Stmt> stmts) {
    arguments = copyIntoExprAllocator(args);
    argumentTypes = copyIntoExprAllocator(argTypes);
    statements = copyIntoExprAllocator(stmts);
  }

  ArrayRef<Bindable> arguments;
  ArrayRef<Type> argumentTypes;
  ArrayRef<Stmt> statements;
};

} // namespace detail
} // namespace edsc
} // namespace mlir

mlir::edsc::ScopedEDSCContext::ScopedEDSCContext() {
  Expr::globalAllocator() = &allocator;
  Bindable::resetIds();
}

mlir::edsc::ScopedEDSCContext::~ScopedEDSCContext() {
  Expr::globalAllocator() = nullptr;
}

mlir::edsc::Expr::Expr() {
  // Initialize with placement new.
  storage = Expr::globalAllocator()->Allocate<detail::ExprStorage>();
  new (storage) detail::ExprStorage(ExprKind::Unbound, {}, {}, {});
}

ExprKind mlir::edsc::Expr::getKind() const { return storage->kind; }

unsigned mlir::edsc::Expr::getId() const {
  return static_cast<ImplType *>(storage)->id;
}

unsigned &mlir::edsc::Expr::newId() {
  static thread_local unsigned id = 0;
  return ++id;
}

Expr mlir::edsc::op::operator+(Expr lhs, Expr rhs) {
  return BinaryExpr(ExprKind::Add, lhs, rhs);
}
Expr mlir::edsc::op::operator-(Expr lhs, Expr rhs) {
  return BinaryExpr(ExprKind::Sub, lhs, rhs);
}
Expr mlir::edsc::op::operator*(Expr lhs, Expr rhs) {
  return BinaryExpr(ExprKind::Mul, lhs, rhs);
}

Expr mlir::edsc::op::operator==(Expr lhs, Expr rhs) {
  return BinaryExpr(ExprKind::EQ, lhs, rhs);
}
Expr mlir::edsc::op::operator!=(Expr lhs, Expr rhs) {
  return BinaryExpr(ExprKind::NE, lhs, rhs);
}
Expr mlir::edsc::op::operator<(Expr lhs, Expr rhs) {
  return BinaryExpr(ExprKind::LT, lhs, rhs);
}
Expr mlir::edsc::op::operator<=(Expr lhs, Expr rhs) {
  return BinaryExpr(ExprKind::LE, lhs, rhs);
}
Expr mlir::edsc::op::operator>(Expr lhs, Expr rhs) {
  return BinaryExpr(ExprKind::GT, lhs, rhs);
}
Expr mlir::edsc::op::operator>=(Expr lhs, Expr rhs) {
  return BinaryExpr(ExprKind::GE, lhs, rhs);
}
Expr mlir::edsc::op::operator&&(Expr lhs, Expr rhs) {
  return BinaryExpr(ExprKind::And, lhs, rhs);
}
Expr mlir::edsc::op::operator||(Expr lhs, Expr rhs) {
  return BinaryExpr(ExprKind::Or, lhs, rhs);
}
Expr mlir::edsc::op::operator!(Expr expr) {
  return UnaryExpr(ExprKind::Negate, expr);
}

llvm::SmallVector<Expr, 8> mlir::edsc::makeNewExprs(unsigned n) {
  llvm::SmallVector<Expr, 8> res;
  res.reserve(n);
  for (auto i = 0; i < n; ++i) {
    res.push_back(Expr());
  }
  return res;
}

static llvm::SmallVector<Expr, 8> makeExprs(edsc_expr_list_t exprList) {
  llvm::SmallVector<Expr, 8> exprs;
  exprs.reserve(exprList.n);
  for (unsigned i = 0; i < exprList.n; ++i) {
    exprs.push_back(Expr(exprList.exprs[i]));
  }
  return exprs;
}

static void fillStmts(edsc_stmt_list_t enclosedStmts,
                      llvm::SmallVector<Stmt, 8> *stmts) {
  stmts->reserve(enclosedStmts.n);
  for (unsigned i = 0; i < enclosedStmts.n; ++i) {
    stmts->push_back(Stmt(enclosedStmts.stmts[i]));
  }
}

Expr mlir::edsc::alloc(llvm::ArrayRef<Expr> sizes, Type memrefType) {
  return VariadicExpr(ExprKind::Alloc, sizes, memrefType);
}

Expr mlir::edsc::dealloc(Expr memref) {
  return UnaryExpr(ExprKind::Dealloc, memref);
}

Stmt mlir::edsc::For(Expr lb, Expr ub, Expr step, ArrayRef<Stmt> stmts) {
  Expr idx;
  return For(Bindable(idx), lb, ub, step, stmts);
}

Stmt mlir::edsc::For(const Bindable &idx, Expr lb, Expr ub, Expr step,
                     ArrayRef<Stmt> stmts) {
  return Stmt(idx, StmtBlockLikeExpr(ExprKind::For, {lb, ub, step}), stmts);
}

Stmt mlir::edsc::For(ArrayRef<Expr> indices, ArrayRef<Expr> lbs,
                     ArrayRef<Expr> ubs, ArrayRef<Expr> steps,
                     ArrayRef<Stmt> enclosedStmts) {
  assert(!indices.empty());
  assert(indices.size() == lbs.size());
  assert(indices.size() == ubs.size());
  assert(indices.size() == steps.size());
  Expr iv = indices.back();
  Stmt curStmt =
      For(Bindable(iv), lbs.back(), ubs.back(), steps.back(), enclosedStmts);
  for (int64_t i = indices.size() - 2; i >= 0; --i) {
    Expr iiv = indices[i];
    curStmt.set(For(Bindable(iiv), lbs[i], ubs[i], steps[i],
                    llvm::ArrayRef<Stmt>{&curStmt, 1}));
  }
  return curStmt;
}

edsc_stmt_t For(edsc_expr_t iv, edsc_expr_t lb, edsc_expr_t ub,
                edsc_expr_t step, edsc_stmt_list_t enclosedStmts) {
  llvm::SmallVector<Stmt, 8> stmts;
  fillStmts(enclosedStmts, &stmts);
  return Stmt(
      For(Expr(iv).cast<Bindable>(), Expr(lb), Expr(ub), Expr(step), stmts));
}

edsc_stmt_t ForNest(edsc_expr_list_t ivs, edsc_expr_list_t lbs,
                    edsc_expr_list_t ubs, edsc_expr_list_t steps,
                    edsc_stmt_list_t enclosedStmts) {
  llvm::SmallVector<Stmt, 8> stmts;
  fillStmts(enclosedStmts, &stmts);
  return Stmt(For(makeExprs(ivs), makeExprs(lbs), makeExprs(ubs),
                  makeExprs(steps), stmts));
}

StmtBlock mlir::edsc::block(ArrayRef<Bindable> args, ArrayRef<Type> argTypes,
                            ArrayRef<Stmt> stmts) {
  assert(args.size() == argTypes.size() &&
         "mismatching number of arguments and argument types");
  return StmtBlock(args, argTypes, stmts);
}

edsc_block_t Block(edsc_stmt_list_t enclosedStmts) {
  llvm::SmallVector<Stmt, 8> stmts;
  fillStmts(enclosedStmts, &stmts);
  return StmtBlock(stmts);
}

Expr mlir::edsc::load(Expr m, ArrayRef<Expr> indices) {
  SmallVector<Expr, 8> exprs;
  exprs.push_back(m);
  exprs.append(indices.begin(), indices.end());
  return VariadicExpr(ExprKind::Load, exprs);
}

edsc_expr_t Load(edsc_indexed_t indexed, edsc_expr_list_t indices) {
  Indexed i(Expr(indexed.base).cast<Bindable>());
  auto exprs = makeExprs(indices);
  Expr res = i(exprs);
  return res;
}

Expr mlir::edsc::store(Expr val, Expr m, ArrayRef<Expr> indices) {
  SmallVector<Expr, 8> exprs;
  exprs.push_back(val);
  exprs.push_back(m);
  exprs.append(indices.begin(), indices.end());
  return VariadicExpr(ExprKind::Store, exprs);
}

edsc_stmt_t Store(edsc_expr_t value, edsc_indexed_t indexed,
                  edsc_expr_list_t indices) {
  Indexed i(Expr(indexed.base).cast<Bindable>());
  auto exprs = makeExprs(indices);
  Indexed loc = i(exprs);
  return Stmt(loc = Expr(value));
}

Expr mlir::edsc::select(Expr cond, Expr lhs, Expr rhs) {
  return TernaryExpr(ExprKind::Select, cond, lhs, rhs);
}

edsc_expr_t Select(edsc_expr_t cond, edsc_expr_t lhs, edsc_expr_t rhs) {
  return select(Expr(cond), Expr(lhs), Expr(rhs));
}

Expr mlir::edsc::vector_type_cast(Expr memrefExpr, Type memrefType) {
  return VariadicExpr(ExprKind::VectorTypeCast, {memrefExpr}, {memrefType});
}

Stmt mlir::edsc::Return(ArrayRef<Expr> values) {
  return VariadicExpr(ExprKind::Return, values);
}

edsc_stmt_t Return(edsc_expr_list_t values) {
  return Stmt(Return(makeExprs(values)));
}

void mlir::edsc::Expr::print(raw_ostream &os) const {
  if (auto unbound = this->dyn_cast<Bindable>()) {
    os << "$" << unbound.getId();
    return;
  } else if (auto un = this->dyn_cast<UnaryExpr>()) {
    switch (un.getKind()) {
    case ExprKind::Negate:
      os << "~";
      break;
    default: {
      os << "unknown_unary";
    }
    }
    os << un.getExpr();
  } else if (auto bin = this->dyn_cast<BinaryExpr>()) {
    os << "(" << bin.getLHS();
    switch (bin.getKind()) {
    case ExprKind::Add:
      os << " + ";
      break;
    case ExprKind::Sub:
      os << " - ";
      break;
    case ExprKind::Mul:
      os << " * ";
      break;
    case ExprKind::Div:
      os << " / ";
      break;
    case ExprKind::LT:
      os << " < ";
      break;
    case ExprKind::LE:
      os << " <= ";
      break;
    case ExprKind::GT:
      os << " > ";
      break;
    case ExprKind::GE:
      os << " >= ";
      break;
    case ExprKind::EQ:
      os << " == ";
      break;
    case ExprKind::NE:
      os << " != ";
      break;
    case ExprKind::And:
      os << " && ";
      break;
    case ExprKind::Or:
      os << " || ";
      break;
    default: {
      os << "unknown_binary";
    }
    }
    os << bin.getRHS() << ")";
    return;
  } else if (auto ter = this->dyn_cast<TernaryExpr>()) {
    switch (ter.getKind()) {
    case ExprKind::Select:
      os << "select(" << ter.getCond() << ", " << ter.getLHS() << ", "
         << ter.getRHS() << ")";
      return;
    default: {
      os << "unknown_ternary";
    }
    }
  } else if (auto nar = this->dyn_cast<VariadicExpr>()) {
    auto exprs = nar.getExprs();
    switch (nar.getKind()) {
    case ExprKind::Load:
      os << "load(" << exprs[0] << "[";
      interleaveComma(ArrayRef<Expr>(exprs.begin() + 1, exprs.size() - 1), os);
      os << "])";
      return;
    case ExprKind::Store:
      os << "store(" << exprs[0] << ", " << exprs[1] << "[";
      interleaveComma(ArrayRef<Expr>(exprs.begin() + 2, exprs.size() - 2), os);
      os << "])";
      return;
    case ExprKind::Return:
      interleaveComma(exprs, os);
      return;
    default: {
      os << "unknown_variadic";
    }
    }
  } else if (auto stmtLikeExpr = this->dyn_cast<StmtBlockLikeExpr>()) {
    auto exprs = stmtLikeExpr.getExprs();
    switch (stmtLikeExpr.getKind()) {
    // We only print the lb, ub and step here, which are the StmtBlockLike
    // part of the `for` StmtBlockLikeExpr.
    case ExprKind::For:
      assert(exprs.size() == 3 && "For StmtBlockLikeExpr expected 3 exprs");
      os << exprs[0] << " to " << exprs[1] << " step " << exprs[2];
      return;
    default: {
      os << "unknown_stmt";
    }
    }
  }
  os << "unknown_kind(" << static_cast<int>(getKind()) << ")";
}

void mlir::edsc::Expr::dump() const { this->print(llvm::errs()); }

std::string mlir::edsc::Expr::str() const {
  std::string res;
  llvm::raw_string_ostream os(res);
  this->print(os);
  return res;
}

llvm::raw_ostream &mlir::edsc::operator<<(llvm::raw_ostream &os,
                                          const Expr &expr) {
  expr.print(os);
  return os;
}

edsc_expr_t makeBindable() { return Bindable(Expr()); }

mlir::edsc::UnaryExpr::UnaryExpr(ExprKind kind, Expr expr)
    : Expr(Expr::globalAllocator()->Allocate<detail::ExprStorage>()) {
  // Initialize with placement new.
  new (storage) detail::ExprStorage(kind, {}, {expr}, {});
}
Expr mlir::edsc::UnaryExpr::getExpr() const {
  return static_cast<ImplType *>(storage)->operands.front();
}

mlir::edsc::BinaryExpr::BinaryExpr(ExprKind kind, Expr lhs, Expr rhs)
    : Expr(Expr::globalAllocator()->Allocate<detail::ExprStorage>()) {
  // Initialize with placement new.
  new (storage) detail::ExprStorage(kind, {}, {lhs, rhs}, {});
}
Expr mlir::edsc::BinaryExpr::getLHS() const {
  return static_cast<ImplType *>(storage)->operands.front();
}
Expr mlir::edsc::BinaryExpr::getRHS() const {
  return static_cast<ImplType *>(storage)->operands.back();
}

mlir::edsc::TernaryExpr::TernaryExpr(ExprKind kind, Expr cond, Expr lhs,
                                     Expr rhs)
    : Expr(Expr::globalAllocator()->Allocate<detail::ExprStorage>()) {
  // Initialize with placement new.
  new (storage) detail::ExprStorage(kind, {}, {cond, lhs, rhs}, {});
}
Expr mlir::edsc::TernaryExpr::getCond() const {
  return static_cast<ImplType *>(storage)->operands[0];
}
Expr mlir::edsc::TernaryExpr::getLHS() const {
  return static_cast<ImplType *>(storage)->operands[1];
}
Expr mlir::edsc::TernaryExpr::getRHS() const {
  return static_cast<ImplType *>(storage)->operands[2];
}

mlir::edsc::VariadicExpr::VariadicExpr(ExprKind kind, ArrayRef<Expr> exprs,
                                       ArrayRef<Type> types)
    : Expr(Expr::globalAllocator()->Allocate<detail::ExprStorage>()) {
  // Initialize with placement new.
  new (storage) detail::ExprStorage(kind, types, exprs, {});
}
ArrayRef<Expr> mlir::edsc::VariadicExpr::getExprs() const {
  return static_cast<ImplType *>(storage)->operands;
}
ArrayRef<Type> mlir::edsc::VariadicExpr::getTypes() const {
  return static_cast<ImplType *>(storage)->resultTypes;
}

mlir::edsc::StmtBlockLikeExpr::StmtBlockLikeExpr(ExprKind kind,
                                                 ArrayRef<Expr> exprs,
                                                 ArrayRef<Type> types)
    : Expr(Expr::globalAllocator()->Allocate<detail::ExprStorage>()) {
  // Initialize with placement new.
  new (storage) detail::ExprStorage(kind, types, exprs, {});
}
ArrayRef<Expr> mlir::edsc::StmtBlockLikeExpr::getExprs() const {
  return static_cast<ImplType *>(storage)->operands;
}

mlir::edsc::Stmt::Stmt(const Bindable &lhs, const Expr &rhs,
                       llvm::ArrayRef<Stmt> enclosedStmts) {
  storage = Expr::globalAllocator()->Allocate<detail::StmtStorage>();
  // Initialize with placement new.
  auto enclosedStmtStorage =
      Expr::globalAllocator()->Allocate<Stmt>(enclosedStmts.size());
  std::uninitialized_copy(enclosedStmts.begin(), enclosedStmts.end(),
                          enclosedStmtStorage);
  new (storage) detail::StmtStorage{
      lhs, rhs, ArrayRef<Stmt>(enclosedStmtStorage, enclosedStmts.size())};
}

mlir::edsc::Stmt::Stmt(const Expr &rhs, llvm::ArrayRef<Stmt> enclosedStmts)
    : Stmt(Bindable(Expr()), rhs, enclosedStmts) {}

edsc_stmt_t makeStmt(edsc_expr_t e) {
  assert(e && "unexpected empty expression");
  return Stmt(Expr(e));
}

Stmt &mlir::edsc::Stmt::operator=(const Expr &expr) {
  Stmt res(Bindable(Expr()), expr, {});
  std::swap(res.storage, this->storage);
  return *this;
}

Expr mlir::edsc::Stmt::getLHS() const {
  return static_cast<ImplType *>(storage)->lhs;
}

Expr mlir::edsc::Stmt::getRHS() const {
  return static_cast<ImplType *>(storage)->rhs;
}

llvm::ArrayRef<Stmt> mlir::edsc::Stmt::getEnclosedStmts() const {
  return storage->enclosedStmts;
}

void mlir::edsc::Stmt::print(raw_ostream &os, Twine indent) const {
  if (!storage) {
    os << "null_storage";
    return;
  }
  auto lhs = getLHS();
  auto rhs = getRHS();

  if (auto stmtExpr = rhs.dyn_cast<StmtBlockLikeExpr>()) {
    switch (stmtExpr.getKind()) {
    case ExprKind::For:
      os << indent << "for(" << lhs << " = " << stmtExpr << ") {";
      os << "\n";
      for (const auto &s : getEnclosedStmts()) {
        if (!s.getRHS().isa<StmtBlockLikeExpr>()) {
          os << indent << "  ";
        }
        s.print(os, indent + "  ");
        os << ";\n";
      }
      os << indent << "}";
      return;
    default: {
      // TODO(ntv): print more statement cases.
      os << "TODO";
    }
    }
  } else {
    os << lhs << " = " << rhs;
  }
}

void mlir::edsc::Stmt::dump() const { this->print(llvm::errs()); }

std::string mlir::edsc::Stmt::str() const {
  std::string res;
  llvm::raw_string_ostream os(res);
  this->print(os);
  return res;
}

llvm::raw_ostream &mlir::edsc::operator<<(llvm::raw_ostream &os,
                                          const Stmt &stmt) {
  stmt.print(os);
  return os;
}

mlir::edsc::StmtBlock::StmtBlock(llvm::ArrayRef<Stmt> stmts)
    : StmtBlock({}, {}, stmts) {}

mlir::edsc::StmtBlock::StmtBlock(llvm::ArrayRef<Bindable> args,
                                 llvm::ArrayRef<Type> argTypes,
                                 llvm::ArrayRef<Stmt> stmts) {
  storage = Expr::globalAllocator()->Allocate<detail::StmtBlockStorage>();
  new (storage) detail::StmtBlockStorage(args, argTypes, stmts);
}

ArrayRef<mlir::edsc::Bindable> mlir::edsc::StmtBlock::getArguments() const {
  return storage->arguments;
}

ArrayRef<Type> mlir::edsc::StmtBlock::getArgumentTypes() const {
  return storage->argumentTypes;
}

ArrayRef<mlir::edsc::Stmt> mlir::edsc::StmtBlock::getBody() const {
  return storage->statements;
}

void mlir::edsc::StmtBlock::print(llvm::raw_ostream &os, Twine indent) const {
  os << indent << "^bb";
  if (!getArgumentTypes().empty())
    os << '(';
  interleaveComma(getArguments(), os);
  if (!getArgumentTypes().empty())
    os << ')';
  os << ":\n";

  for (auto stmt : getBody())
    stmt.print(os, indent + "  ");
}

std::string mlir::edsc::StmtBlock::str() const {
  std::string result;
  llvm::raw_string_ostream os(result);
  print(os, "");
  return result;
}

Indexed mlir::edsc::Indexed::operator()(llvm::ArrayRef<Expr> indices) {
  Indexed res(base);
  res.indices = llvm::SmallVector<Expr, 4>(indices.begin(), indices.end());
  return res;
}

// NOLINTNEXTLINE: unconventional-assign-operator
Stmt mlir::edsc::Indexed::operator=(Expr expr) {
  return Stmt(store(expr, base, indices));
}

edsc_indexed_t makeIndexed(edsc_expr_t expr) {
  return edsc_indexed_t{expr, edsc_expr_list_t{nullptr, 0}};
}

edsc_indexed_t index(edsc_indexed_t indexed, edsc_expr_list_t indices) {
  return edsc_indexed_t{indexed.base, indices};
}

mlir_type_t makeScalarType(mlir_context_t context, const char *name,
                           unsigned bitwidth) {
  mlir::MLIRContext *c = reinterpret_cast<mlir::MLIRContext *>(context);
  mlir_type_t res =
      llvm::StringSwitch<mlir_type_t>(name)
          .Case("bf16",
                mlir_type_t{mlir::FloatType::getBF16(c).getAsOpaquePointer()})
          .Case("f16",
                mlir_type_t{mlir::FloatType::getF16(c).getAsOpaquePointer()})
          .Case("f32",
                mlir_type_t{mlir::FloatType::getF32(c).getAsOpaquePointer()})
          .Case("f64",
                mlir_type_t{mlir::FloatType::getF64(c).getAsOpaquePointer()})
          .Case("index",
                mlir_type_t{mlir::IndexType::get(c).getAsOpaquePointer()})
          .Case("i",
                mlir_type_t{
                    mlir::IntegerType::get(bitwidth, c).getAsOpaquePointer()})
          .Default(mlir_type_t{nullptr});
  if (!res) {
    llvm_unreachable("Invalid type specifier");
  }
  return res;
}

mlir_type_t makeMemRefType(mlir_context_t context, mlir_type_t elemType,
                           int64_list_t sizes) {
  auto t = mlir::MemRefType::get(
      llvm::ArrayRef<int64_t>(sizes.values, sizes.n),
      mlir::Type::getFromOpaquePointer(elemType),
      {mlir::AffineMap::getMultiDimIdentityMap(
          sizes.n, reinterpret_cast<mlir::MLIRContext *>(context))},
      0);
  return mlir_type_t{t.getAsOpaquePointer()};
}

mlir_type_t makeFunctionType(mlir_context_t context, mlir_type_list_t inputs,
                             mlir_type_list_t outputs) {
  llvm::SmallVector<mlir::Type, 8> ins(inputs.n), outs(outputs.n);
  for (unsigned i = 0; i < inputs.n; ++i) {
    ins[i] = mlir::Type::getFromOpaquePointer(inputs.types[i]);
  }
  for (unsigned i = 0; i < outputs.n; ++i) {
    outs[i] = mlir::Type::getFromOpaquePointer(outputs.types[i]);
  }
  auto ft = mlir::FunctionType::get(
      ins, outs, reinterpret_cast<mlir::MLIRContext *>(context));
  return mlir_type_t{ft.getAsOpaquePointer()};
}

unsigned getFunctionArity(mlir_func_t function) {
  auto *f = reinterpret_cast<mlir::Function *>(function);
  return f->getNumArguments();
}
