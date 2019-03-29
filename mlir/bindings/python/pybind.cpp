#include "third_party/llvm/llvm/include/llvm/ADT/SmallVector.h"
#include "third_party/llvm/llvm/include/llvm/ADT/StringRef.h"
#include "third_party/llvm/llvm/include/llvm/IR/Module.h"
#include "third_party/llvm/llvm/include/llvm/Support/TargetSelect.h"
#include "third_party/llvm/llvm/include/llvm/Support/raw_ostream.h"
#include <cstddef>

#include "third_party/llvm/llvm/projects/google_mlir/include/mlir-c/Core.h"
#include "third_party/llvm/llvm/projects/google_mlir/include/mlir/EDSC/MLIREmitter.h"
#include "third_party/llvm/llvm/projects/google_mlir/include/mlir/EDSC/Types.h"
#include "third_party/llvm/llvm/projects/google_mlir/include/mlir/ExecutionEngine/ExecutionEngine.h"
#include "third_party/llvm/llvm/projects/google_mlir/include/mlir/IR/BuiltinOps.h"
#include "third_party/llvm/llvm/projects/google_mlir/include/mlir/IR/Module.h"
#include "third_party/llvm/llvm/projects/google_mlir/include/mlir/Pass/Pass.h"
#include "third_party/llvm/llvm/projects/google_mlir/include/mlir/Target/LLVMIR.h"
#include "third_party/llvm/llvm/projects/google_mlir/include/mlir/Transforms/Passes.h"
#include "pybind11/pybind11.h"
#include "pybind11/pytypes.h"
#include "pybind11/stl.h"

#include "mlir/IR/Function.h"
#include "mlir/IR/Types.h"

static bool inited = [] {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  return true;
}();

namespace mlir {
namespace edsc {
namespace python {

namespace py = pybind11;

struct PythonAttribute;
struct PythonBindable;
struct PythonExpr;
struct PythonStmt;
struct PythonBlock;

struct PythonFunction {
  PythonFunction() : function{nullptr} {}
  PythonFunction(mlir_func_t f) : function{f} {}
  PythonFunction(mlir::Function *f) : function{f} {}
  operator mlir_func_t() { return function; }
  std::string str() {
    mlir::Function *f = reinterpret_cast<mlir::Function *>(function);
    std::string res;
    llvm::raw_string_ostream os(res);
    f->print(os);
    return res;
  }

  // If the function does not yet have an entry block, i.e. if it is a function
  // declaration, add the entry block, transforming the declaration into a
  // definition.  Return true if the block was added, false otherwise.
  bool define() {
    auto *f = reinterpret_cast<mlir::Function *>(function);
    if (!f->getBlocks().empty())
      return false;

    f->addEntryBlock();
    return true;
  }

  mlir_func_t function;
};

struct PythonType {
  PythonType() : type{nullptr} {}
  PythonType(mlir_type_t t) : type{t} {}
  operator mlir_type_t() { return type; }
  std::string str() {
    mlir::Type f = mlir::Type::getFromOpaquePointer(type);
    std::string res;
    llvm::raw_string_ostream os(res);
    f.print(os);
    return res;
  }
  mlir_type_t type;
};

/// Trivial C++ wrappers make use of the EDSC C API.
struct PythonMLIRModule {
  PythonMLIRModule() : mlirContext(), module(new mlir::Module(&mlirContext)) {}

  PythonType makeScalarType(const std::string &mlirElemType,
                            unsigned bitwidth) {
    return ::makeScalarType(mlir_context_t{&mlirContext}, mlirElemType.c_str(),
                            bitwidth);
  }
  PythonType makeMemRefType(PythonType elemType, std::vector<int64_t> sizes) {
    return ::makeMemRefType(mlir_context_t{&mlirContext}, elemType,
                            int64_list_t{sizes.data(), sizes.size()});
  }
  PythonType makeIndexType() {
    return ::makeIndexType(mlir_context_t{&mlirContext});
  }

  // Declare a function with the given name, input and output types but do not
  // define it.
  PythonFunction declareFunction(const std::string &name,
                                 std::vector<PythonType> &inputTypes,
                                 std::vector<PythonType> &outputTypes) {
    std::vector<mlir_type_t> ins(inputTypes.begin(), inputTypes.end());
    std::vector<mlir_type_t> outs(outputTypes.begin(), outputTypes.end());
    auto funcType = ::makeFunctionType(
        mlir_context_t{&mlirContext}, mlir_type_list_t{ins.data(), ins.size()},
        mlir_type_list_t{outs.data(), outs.size()});
    auto *func = new mlir::Function(
        UnknownLoc::get(&mlirContext), name,
        mlir::Type::getFromOpaquePointer(funcType).cast<FunctionType>());
    module->getFunctions().push_back(func);
    return func;
  }

  // Define a function with the given name, input and output types.
  PythonFunction makeFunction(const std::string &name,
                              std::vector<PythonType> &inputTypes,
                              std::vector<PythonType> &outputTypes) {
    auto declaration = declareFunction(name, inputTypes, outputTypes);
    declaration.define();
    return declaration;
  }

  // Create a custom op given its name and arguments.
  PythonExpr op(const std::string &name, PythonType type,
                const py::list &arguments, const py::list &successors,
                py::kwargs attributes);

  // Create an integer attribute.
  PythonAttribute integerAttr(PythonType type, int64_t value);

  // Create a boolean attribute.
  PythonAttribute boolAttr(bool value);

  void compile() {
    auto created = mlir::ExecutionEngine::create(module.get());
    llvm::handleAllErrors(created.takeError(),
                          [](const llvm::ErrorInfoBase &b) {
                            b.log(llvm::errs());
                            assert(false);
                          });
    engine = std::move(*created);
  }

  std::string getIR() {
    std::string res;
    llvm::raw_string_ostream os(res);
    module->print(os);
    return res;
  }

  uint64_t getEngineAddress() {
    assert(engine && "module must be compiled into engine first");
    return reinterpret_cast<uint64_t>(reinterpret_cast<void *>(engine.get()));
  }

  PythonFunction getNamedFunction(const std::string &name) {
    return module->getNamedFunction(name);
  }

private:
  mlir::MLIRContext mlirContext;
  // One single module in a python-exposed MLIRContext for now.
  std::unique_ptr<mlir::Module> module;
  std::unique_ptr<mlir::ExecutionEngine> engine;
};

struct ContextManager {
  void enter() { context = new ScopedEDSCContext(); }
  void exit(py::object, py::object, py::object) {
    delete context;
    context = nullptr;
  }
  mlir::edsc::ScopedEDSCContext *context;
};

struct PythonExpr {
  PythonExpr() : expr{nullptr} {}
  PythonExpr(const PythonBindable &bindable);
  PythonExpr(const edsc_expr_t &expr) : expr{expr} {}
  operator edsc_expr_t() { return expr; }
  std::string str() {
    assert(expr && "unexpected empty expr");
    return Expr(*this).str();
  }
  edsc_expr_t expr;
};

struct PythonBindable : public PythonExpr {
  explicit PythonBindable(const PythonType &type)
      : PythonExpr(edsc_expr_t{makeBindable(type.type)}) {}
  PythonBindable(PythonExpr expr) : PythonExpr(expr) {
    assert(Expr(expr).isa<Bindable>() && "Expected Bindable");
  }
  std::string str() {
    assert(expr && "unexpected empty expr");
    return Expr(expr).str();
  }
};

struct PythonStmt {
  PythonStmt() : stmt{nullptr} {}
  PythonStmt(const edsc_stmt_t &stmt) : stmt{stmt} {}
  PythonStmt(const PythonExpr &e) : stmt{makeStmt(e.expr)} {}
  operator edsc_stmt_t() { return stmt; }
  std::string str() {
    assert(stmt && "unexpected empty stmt");
    return Stmt(stmt).str();
  }
  edsc_stmt_t stmt;
};

struct PythonBlock {
  PythonBlock() : blk{nullptr} {}
  PythonBlock(const edsc_block_t &other) : blk{other} {}
  PythonBlock(const PythonBlock &other) = default;
  operator edsc_block_t() { return blk; }
  std::string str() {
    assert(blk && "unexpected empty block");
    return StmtBlock(blk).str();
  }

  PythonBlock set(const py::list &stmts);

  edsc_block_t blk;
};

struct PythonAttribute {
  PythonAttribute() : attr(nullptr) {}
  PythonAttribute(const mlir_attr_t &a) : attr(a) {}
  PythonAttribute(const PythonAttribute &other) = default;
  operator mlir_attr_t() { return attr; }

  std::string str() {
    if (!attr)
      return "##null attr##";

    std::string res;
    llvm::raw_string_ostream os(res);
    Attribute::getFromOpaquePointer(reinterpret_cast<const void *>(attr))
        .print(os);
    return res;
  }

  mlir_attr_t attr;
};

struct PythonIndexed : public edsc_indexed_t {
  PythonIndexed(PythonExpr e) : edsc_indexed_t{makeIndexed(e)} {}
  PythonIndexed(PythonBindable b) : edsc_indexed_t{makeIndexed(b)} {}
  operator PythonExpr() { return PythonExpr(base); }
};

struct PythonMaxExpr {
  PythonMaxExpr() : expr(nullptr) {}
  PythonMaxExpr(const edsc_max_expr_t &e) : expr(e) {}
  operator edsc_max_expr_t() { return expr; }

  edsc_max_expr_t expr;
};

struct PythonMinExpr {
  PythonMinExpr() : expr(nullptr) {}
  PythonMinExpr(const edsc_min_expr_t &e) : expr(e) {}
  operator edsc_min_expr_t() { return expr; }

  edsc_min_expr_t expr;
};

struct MLIRFunctionEmitter {
  MLIRFunctionEmitter(PythonFunction f)
      : currentFunction(reinterpret_cast<mlir::Function *>(f.function)),
        currentBuilder(currentFunction),
        emitter(&currentBuilder, currentFunction->getLoc()) {}

  PythonExpr bindConstantBF16(double value);
  PythonExpr bindConstantF16(float value);
  PythonExpr bindConstantF32(float value);
  PythonExpr bindConstantF64(double value);
  PythonExpr bindConstantInt(int64_t value, unsigned bitwidth);
  PythonExpr bindConstantIndex(int64_t value);
  PythonExpr bindConstantFunction(PythonFunction func);
  PythonExpr bindFunctionArgument(unsigned pos);
  py::list bindFunctionArguments();
  py::list bindFunctionArgumentView(unsigned pos);
  py::list bindMemRefShape(PythonExpr boundMemRef);
  py::list bindIndexedMemRefShape(PythonIndexed boundMemRef) {
    return bindMemRefShape(boundMemRef.base);
  }
  py::list bindMemRefView(PythonExpr boundMemRef);
  py::list bindIndexedMemRefView(PythonIndexed boundMemRef) {
    return bindMemRefView(boundMemRef.base);
  }
  void emit(PythonStmt stmt);
  void emitBlock(PythonBlock block);
  void emitBlockBody(PythonBlock block);

private:
  mlir::Function *currentFunction;
  mlir::FuncBuilder currentBuilder;
  mlir::edsc::MLIREmitter emitter;
  edsc_mlir_emitter_t c_emitter;
};

template <typename ListTy, typename PythonTy, typename Ty>
ListTy makeCList(SmallVectorImpl<Ty> &owning, const py::list &list) {
  for (auto &inp : list) {
    owning.push_back(Ty{inp.cast<PythonTy>()});
  }
  return ListTy{owning.data(), owning.size()};
}

static edsc_stmt_list_t makeCStmts(llvm::SmallVectorImpl<edsc_stmt_t> &owning,
                                   const py::list &stmts) {
  return makeCList<edsc_stmt_list_t, PythonStmt>(owning, stmts);
}

static edsc_expr_list_t makeCExprs(llvm::SmallVectorImpl<edsc_expr_t> &owning,
                                   const py::list &exprs) {
  return makeCList<edsc_expr_list_t, PythonExpr>(owning, exprs);
}

static mlir_type_list_t makeCTypes(llvm::SmallVectorImpl<mlir_type_t> &owning,
                                   const py::list &types) {
  return makeCList<mlir_type_list_t, PythonType>(owning, types);
}

static edsc_block_list_t
makeCBlocks(llvm::SmallVectorImpl<edsc_block_t> &owning,
            const py::list &blocks) {
  return makeCList<edsc_block_list_t, PythonBlock>(owning, blocks);
}

PythonExpr::PythonExpr(const PythonBindable &bindable) : expr{bindable.expr} {}

PythonExpr MLIRFunctionEmitter::bindConstantBF16(double value) {
  return ::bindConstantBF16(edsc_mlir_emitter_t{&emitter}, value);
}

PythonExpr MLIRFunctionEmitter::bindConstantF16(float value) {
  return ::bindConstantF16(edsc_mlir_emitter_t{&emitter}, value);
}

PythonExpr MLIRFunctionEmitter::bindConstantF32(float value) {
  return ::bindConstantF32(edsc_mlir_emitter_t{&emitter}, value);
}

PythonExpr MLIRFunctionEmitter::bindConstantF64(double value) {
  return ::bindConstantF64(edsc_mlir_emitter_t{&emitter}, value);
}

PythonExpr MLIRFunctionEmitter::bindConstantInt(int64_t value,
                                                unsigned bitwidth) {
  return ::bindConstantInt(edsc_mlir_emitter_t{&emitter}, value, bitwidth);
}

PythonExpr MLIRFunctionEmitter::bindConstantIndex(int64_t value) {
  return ::bindConstantIndex(edsc_mlir_emitter_t{&emitter}, value);
}

PythonExpr MLIRFunctionEmitter::bindConstantFunction(PythonFunction func) {
  return ::bindConstantFunction(edsc_mlir_emitter_t{&emitter}, func);
}

PythonExpr MLIRFunctionEmitter::bindFunctionArgument(unsigned pos) {
  return ::bindFunctionArgument(edsc_mlir_emitter_t{&emitter},
                                mlir_func_t{currentFunction}, pos);
}

PythonExpr getPythonType(edsc_expr_t e) { return PythonExpr(e); }

template <typename T> py::list makePyList(llvm::ArrayRef<T> owningResults) {
  py::list res;
  for (auto e : owningResults) {
    res.append(getPythonType(e));
  }
  return res;
}

py::list MLIRFunctionEmitter::bindFunctionArguments() {
  auto arity = getFunctionArity(mlir_func_t{currentFunction});
  llvm::SmallVector<edsc_expr_t, 8> owningResults(arity);
  edsc_expr_list_t results{owningResults.data(), owningResults.size()};
  ::bindFunctionArguments(edsc_mlir_emitter_t{&emitter},
                          mlir_func_t{currentFunction}, &results);
  return makePyList(ArrayRef<edsc_expr_t>{owningResults});
}

py::list MLIRFunctionEmitter::bindMemRefShape(PythonExpr boundMemRef) {
  auto rank = getBoundMemRefRank(edsc_mlir_emitter_t{&emitter}, boundMemRef);
  llvm::SmallVector<edsc_expr_t, 8> owningShapes(rank);
  edsc_expr_list_t resultShapes{owningShapes.data(), owningShapes.size()};
  ::bindMemRefShape(edsc_mlir_emitter_t{&emitter}, boundMemRef, &resultShapes);
  return makePyList(ArrayRef<edsc_expr_t>{owningShapes});
}

py::list MLIRFunctionEmitter::bindMemRefView(PythonExpr boundMemRef) {
  auto rank = getBoundMemRefRank(edsc_mlir_emitter_t{&emitter}, boundMemRef);
  // Own the PythonExpr for the arg as well as all its dims.
  llvm::SmallVector<edsc_expr_t, 8> owningLbs(rank);
  llvm::SmallVector<edsc_expr_t, 8> owningUbs(rank);
  llvm::SmallVector<edsc_expr_t, 8> owningSteps(rank);
  edsc_expr_list_t resultLbs{owningLbs.data(), owningLbs.size()};
  edsc_expr_list_t resultUbs{owningUbs.data(), owningUbs.size()};
  edsc_expr_list_t resultSteps{owningSteps.data(), owningSteps.size()};
  ::bindMemRefView(edsc_mlir_emitter_t{&emitter}, boundMemRef, &resultLbs,
                   &resultUbs, &resultSteps);
  py::list res;
  res.append(makePyList(ArrayRef<edsc_expr_t>{owningLbs}));
  res.append(makePyList(ArrayRef<edsc_expr_t>{owningUbs}));
  res.append(makePyList(ArrayRef<edsc_expr_t>{owningSteps}));
  return res;
}

void MLIRFunctionEmitter::emit(PythonStmt stmt) {
  emitter.emitStmt(Stmt(stmt));
}

void MLIRFunctionEmitter::emitBlock(PythonBlock block) {
  emitter.emitBlock(StmtBlock(block));
}

void MLIRFunctionEmitter::emitBlockBody(PythonBlock block) {
  emitter.emitStmts(StmtBlock(block).getBody());
}

PythonExpr PythonMLIRModule::op(const std::string &name, PythonType type,
                                const py::list &arguments,
                                const py::list &successors,
                                py::kwargs attributes) {
  SmallVector<edsc_expr_t, 8> owningExprs;
  SmallVector<edsc_block_t, 4> owningBlocks;
  SmallVector<mlir_named_attr_t, 4> owningAttrs;
  SmallVector<std::string, 4> owningAttrNames;

  owningAttrs.reserve(attributes.size());
  owningAttrNames.reserve(attributes.size());
  for (const auto &kvp : attributes) {
    owningAttrNames.push_back(kvp.first.str());
    auto value = kvp.second.cast<PythonAttribute>();
    owningAttrs.push_back({owningAttrNames.back().c_str(), value});
  }

  return PythonExpr(::Op(mlir_context_t(&mlirContext), name.c_str(), type,
                         makeCExprs(owningExprs, arguments),
                         makeCBlocks(owningBlocks, successors),
                         {owningAttrs.data(), owningAttrs.size()}));
}

PythonAttribute PythonMLIRModule::integerAttr(PythonType type, int64_t value) {
  return PythonAttribute(::makeIntegerAttr(type, value));
}

PythonAttribute PythonMLIRModule::boolAttr(bool value) {
  return PythonAttribute(::makeBoolAttr(&mlirContext, value));
}

PythonBlock PythonBlock::set(const py::list &stmts) {
  SmallVector<edsc_stmt_t, 8> owning;
  ::BlockSetBody(blk, makeCStmts(owning, stmts));
  return *this;
}

PythonExpr dispatchCall(py::args args, py::kwargs kwargs) {
  assert(args.size() != 0);
  llvm::SmallVector<edsc_expr_t, 8> exprs;
  exprs.reserve(args.size());
  for (auto arg : args) {
    exprs.push_back(arg.cast<PythonExpr>());
  }

  edsc_expr_list_t operands{exprs.data() + 1, exprs.size() - 1};

  if (kwargs && kwargs.contains("result")) {
    for (const auto &kvp : kwargs) {
      if (static_cast<std::string>(kvp.first.str()) == "result")
        return ::Call1(exprs.front(), kvp.second.cast<PythonType>(), operands);
    }
  }
  return ::Call0(exprs.front(), operands);
}

PYBIND11_MODULE(pybind, m) {
  m.doc() =
      "Python bindings for MLIR Embedded Domain-Specific Components (EDSCs)";
  m.def("version", []() { return "EDSC Python extensions v0.0"; });
  m.def("initContext",
        []() { return static_cast<void *>(new ScopedEDSCContext()); });
  m.def("deleteContext",
        [](void *ctx) { delete reinterpret_cast<ScopedEDSCContext *>(ctx); });

  m.def("Block", [](const py::list &args, const py::list &stmts) {
    SmallVector<edsc_stmt_t, 8> owning;
    SmallVector<edsc_expr_t, 8> owningArgs;
    return PythonBlock(
        ::Block(makeCExprs(owningArgs, args), makeCStmts(owning, stmts)));
  });
  m.def("Block", [](const py::list &stmts) {
    SmallVector<edsc_stmt_t, 8> owning;
    edsc_expr_list_t args{nullptr, 0};
    return PythonBlock(::Block(args, makeCStmts(owning, stmts)));
  });
  m.def(
      "Branch",
      [](PythonBlock destination, const py::list &operands) {
        SmallVector<edsc_expr_t, 8> owning;
        return PythonStmt(::Branch(destination, makeCExprs(owning, operands)));
      },
      py::arg("destination"), py::arg("operands") = py::list());
  m.def("CondBranch",
        [](PythonExpr condition, PythonBlock trueDestination,
           const py::list &trueOperands, PythonBlock falseDestination,
           const py::list &falseOperands) {
          SmallVector<edsc_expr_t, 8> owningTrue;
          SmallVector<edsc_expr_t, 8> owningFalse;
          return PythonStmt(::CondBranch(
              condition, trueDestination, makeCExprs(owningTrue, trueOperands),
              falseDestination, makeCExprs(owningFalse, falseOperands)));
        });
  m.def("CondBranch", [](PythonExpr condition, PythonBlock trueDestination,
                         PythonBlock falseDestination) {
    edsc_expr_list_t emptyList;
    emptyList.exprs = nullptr;
    emptyList.n = 0;
    return PythonStmt(::CondBranch(condition, trueDestination, emptyList,
                                   falseDestination, emptyList));
  });
  m.def("For", [](const py::list &ivs, const py::list &lbs, const py::list &ubs,
                  const py::list &steps, const py::list &stmts) {
    SmallVector<edsc_expr_t, 8> owningIVs;
    SmallVector<edsc_expr_t, 8> owningLBs;
    SmallVector<edsc_expr_t, 8> owningUBs;
    SmallVector<edsc_expr_t, 8> owningSteps;
    SmallVector<edsc_stmt_t, 8> owningStmts;
    return PythonStmt(
        ::ForNest(makeCExprs(owningIVs, ivs), makeCExprs(owningLBs, lbs),
                  makeCExprs(owningUBs, ubs), makeCExprs(owningSteps, steps),
                  makeCStmts(owningStmts, stmts)));
  });
  m.def("Max", [](const py::list &args) {
    SmallVector<edsc_expr_t, 8> owning;
    return PythonMaxExpr(::Max(makeCExprs(owning, args)));
  });
  m.def("Min", [](const py::list &args) {
    SmallVector<edsc_expr_t, 8> owning;
    return PythonMinExpr(::Min(makeCExprs(owning, args)));
  });
  m.def("For", [](PythonExpr iv, PythonExpr lb, PythonExpr ub, PythonExpr step,
                  const py::list &stmts) {
    SmallVector<edsc_stmt_t, 8> owning;
    return PythonStmt(::For(iv, lb, ub, step, makeCStmts(owning, stmts)));
  });
  m.def("For", [](PythonExpr iv, PythonMaxExpr lb, PythonMinExpr ub,
                  PythonExpr step, const py::list &stmts) {
    SmallVector<edsc_stmt_t, 8> owning;
    return PythonStmt(::MaxMinFor(iv, lb, ub, step, makeCStmts(owning, stmts)));
  });
  m.def("Select", [](PythonExpr cond, PythonExpr e1, PythonExpr e2) {
    return PythonExpr(::Select(cond, e1, e2));
  });
  m.def("Return", []() {
    return PythonStmt(::Return(edsc_expr_list_t{nullptr, 0}));
  });
  m.def("Return", [](const py::list &returns) {
    SmallVector<edsc_expr_t, 8> owningExprs;
    return PythonStmt(::Return(makeCExprs(owningExprs, returns)));
  });
  m.def("ConstantInteger", [](PythonType type, int64_t value) {
    return PythonExpr(::ConstantInteger(type, value));
  });

#define DEFINE_PYBIND_BINARY_OP(PYTHON_NAME, C_NAME)                           \
  m.def(PYTHON_NAME, [](PythonExpr e1, PythonExpr e2) {                        \
    return PythonExpr(::C_NAME(e1, e2));                                       \
  });

  DEFINE_PYBIND_BINARY_OP("Add", Add);
  DEFINE_PYBIND_BINARY_OP("Mul", Mul);
  DEFINE_PYBIND_BINARY_OP("Sub", Sub);
  DEFINE_PYBIND_BINARY_OP("Div", Div);
  DEFINE_PYBIND_BINARY_OP("Rem", Rem);
  DEFINE_PYBIND_BINARY_OP("FloorDiv", FloorDiv);
  DEFINE_PYBIND_BINARY_OP("CeilDiv", CeilDiv);
  DEFINE_PYBIND_BINARY_OP("LT", LT);
  DEFINE_PYBIND_BINARY_OP("LE", LE);
  DEFINE_PYBIND_BINARY_OP("GT", GT);
  DEFINE_PYBIND_BINARY_OP("GE", GE);
  DEFINE_PYBIND_BINARY_OP("EQ", EQ);
  DEFINE_PYBIND_BINARY_OP("NE", NE);
  DEFINE_PYBIND_BINARY_OP("And", And);
  DEFINE_PYBIND_BINARY_OP("Or", Or);

#undef DEFINE_PYBIND_BINARY_OP

#define DEFINE_PYBIND_UNARY_OP(PYTHON_NAME, C_NAME)                            \
  m.def(PYTHON_NAME, [](PythonExpr e1) { return PythonExpr(::C_NAME(e1)); });

  DEFINE_PYBIND_UNARY_OP("Negate", Negate);

#undef DEFINE_PYBIND_UNARY_OP

  py::class_<PythonFunction>(m, "Function",
                             "Wrapping class for mlir::Function.")
      .def(py::init<PythonFunction>())
      .def("__str__", &PythonFunction::str)
      .def("define", &PythonFunction::define,
           "Adds a body to the function if it does not already have one.  "
           "Returns true if the body was added");

  py::class_<PythonBlock>(m, "StmtBlock",
                          "Wrapping class for mlir::edsc::StmtBlock")
      .def(py::init<PythonBlock>())
      .def("set", &PythonBlock::set)
      .def("__str__", &PythonBlock::str);

  py::class_<PythonAttribute>(m, "Attribute",
                              "Wrapping class for mlir::Attribute")
      .def(py::init<PythonAttribute>())
      .def("__str__", &PythonAttribute::str);

  py::class_<PythonType>(m, "Type", "Wrapping class for mlir::Type.")
      .def(py::init<PythonType>())
      .def("__str__", &PythonType::str);

  py::class_<PythonMLIRModule>(
      m, "MLIRModule",
      "An MLIRModule is the abstraction that owns the allocations to support "
      "compilation of a single mlir::Module into an ExecutionEngine backed by "
      "the LLVM ORC JIT. A typical flow consists in creating an MLIRModule, "
      "adding functions, compiling the module to obtain an ExecutionEngine on "
      "which named functions may be called. For now the only means to retrieve "
      "the ExecutionEngine is by calling `get_engine_address`. This mode of "
      "execution is limited to passing the pointer to C++ where the function "
      "is called. Extending the API to allow calling JIT compiled functions "
      "directly require integration with a tensor library (e.g. numpy). This "
      "is left as the prerogative of libraries and frameworks for now.")
      .def(py::init<>())
      .def("op", &PythonMLIRModule::op, py::arg("name"), py::arg("type"),
           py::arg("arguments"), py::arg("successors") = py::list(),
           "Creates a new expression identified by its canonical name.")
      .def("boolAttr", &PythonMLIRModule::boolAttr,
           "Creates an mlir::BoolAttr with the given value")
      .def(
          "integerAttr", &PythonMLIRModule::integerAttr,
          "Creates an mlir::IntegerAttr of the given type with the given value "
          "in the context associated with this MLIR module.")
      .def("declare_function", &PythonMLIRModule::declareFunction,
           "Declares a new mlir::Function in the current mlir::Module.  The "
           "function has no definition and can be linked to an external "
           "library.")
      .def("make_function", &PythonMLIRModule::makeFunction,
           "Defines a new mlir::Function in the current mlir::Module.")
      .def(
          "make_scalar_type",
          [](PythonMLIRModule &instance, const std::string &type,
             unsigned bitwidth) {
            return instance.makeScalarType(type, bitwidth);
          },
          py::arg("type"), py::arg("bitwidth") = 0,
          "Returns a scalar mlir::Type using the following convention:\n"
          "  - makeScalarType(c, \"bf16\") return an "
          "`mlir::FloatType::getBF16`\n"
          "  - makeScalarType(c, \"f16\") return an `mlir::FloatType::getF16`\n"
          "  - makeScalarType(c, \"f32\") return an `mlir::FloatType::getF32`\n"
          "  - makeScalarType(c, \"f64\") return an `mlir::FloatType::getF64`\n"
          "  - makeScalarType(c, \"index\") return an `mlir::IndexType::get`\n"
          "  - makeScalarType(c, \"i\", bitwidth) return an "
          "`mlir::IntegerType::get(bitwidth)`\n\n"
          " No other combinations are currently supported.")
      .def("make_memref_type", &PythonMLIRModule::makeMemRefType,
           "Returns an mlir::MemRefType of an elemental scalar. -1 is used to "
           "denote symbolic dimensions in the resulting memref shape.")
      .def("make_index_type", &PythonMLIRModule::makeIndexType,
           "Returns an mlir::IndexType")
      .def("compile", &PythonMLIRModule::compile,
           "Compiles the mlir::Module to LLVMIR a creates new opaque "
           "ExecutionEngine backed by the ORC JIT.")
      .def("get_ir", &PythonMLIRModule::getIR,
           "Returns a dump of the MLIR representation of the module. This is "
           "used for serde to support out-of-process execution as well as "
           "debugging purposes.")
      .def("get_engine_address", &PythonMLIRModule::getEngineAddress,
           "Returns the address of the compiled ExecutionEngine. This is used "
           "for in-process execution.")
      .def("__str__", &PythonMLIRModule::getIR,
           "Get the string representation of the module");

  py::class_<ContextManager>(
      m, "ContextManager",
      "An EDSC context manager is the memory arena containing all the EDSC "
      "allocations.\nUsage:\n\n"
      "with E.ContextManager() as _:\n  i = E.Expr(E.Bindable())\n  ...")
      .def(py::init<>())
      .def("__enter__", &ContextManager::enter)
      .def("__exit__", &ContextManager::exit);

  py::class_<MLIRFunctionEmitter>(
      m, "MLIRFunctionEmitter",
      "An MLIRFunctionEmitter is used to fill an empty function body. This is "
      "a staged process:\n"
      "  1. create or retrieve an mlir::Function `f` with an empty body;\n"
      "  2. make an `MLIRFunctionEmitter(f)` to build the current function;\n"
      "  3. create leaf Expr that are either Bindable or already Expr that are"
      "     bound to constants and function arguments by using methods of "
      "     `MLIRFunctionEmitter`;\n"
      "  4. build the function body using Expr, Indexed and Stmt;\n"
      "  5. emit the MLIR to implement the function body.")
      .def(py::init<PythonFunction>())
      .def("bind_constant_bf16", &MLIRFunctionEmitter::bindConstantBF16)
      .def("bind_constant_f16", &MLIRFunctionEmitter::bindConstantF16)
      .def("bind_constant_f32", &MLIRFunctionEmitter::bindConstantF32)
      .def("bind_constant_f64", &MLIRFunctionEmitter::bindConstantF64)
      .def("bind_constant_int", &MLIRFunctionEmitter::bindConstantInt)
      .def("bind_constant_index", &MLIRFunctionEmitter::bindConstantIndex)
      .def("bind_constant_function", &MLIRFunctionEmitter::bindConstantFunction)
      .def("bind_function_argument", &MLIRFunctionEmitter::bindFunctionArgument,
           "Returns an Expr that has been bound to a positional argument in "
           "the current Function.")
      .def("bind_function_arguments",
           &MLIRFunctionEmitter::bindFunctionArguments,
           "Returns a list of Expr where each Expr has been bound to the "
           "corresponding positional argument in the current Function.")
      .def("bind_memref_shape", &MLIRFunctionEmitter::bindMemRefShape,
           "Returns a list of Expr where each Expr has been bound to the "
           "corresponding dimension of the memref.")
      .def("bind_memref_view", &MLIRFunctionEmitter::bindMemRefView,
           "Returns three lists (lower bound, upper bound and step) of Expr "
           "where each triplet of Expr has been bound to the minimal offset, "
           "extent and stride of the corresponding dimension of the memref.")
      .def("bind_indexed_shape", &MLIRFunctionEmitter::bindIndexedMemRefShape,
           "Same as bind_memref_shape but returns a list of `Indexed` that "
           "support load and store operations")
      .def("bind_indexed_view", &MLIRFunctionEmitter::bindIndexedMemRefView,
           "Same as bind_memref_view but returns lists of `Indexed` that "
           "support load and store operations")
      .def("emit", &MLIRFunctionEmitter::emit,
           "Emits the MLIR for the EDSC expressions and statements in the "
           "current function body.")
      .def("emit", &MLIRFunctionEmitter::emitBlock,
           "Emits the MLIR for the EDSC statements into a new block")
      .def("emit_inplace", &MLIRFunctionEmitter::emitBlockBody,
           "Emits the MLIR for the EDSC statements contained in a EDSC block "
           "into the current function body without creating a new block");

  py::class_<PythonExpr>(m, "Expr", "Wrapping class for mlir::edsc::Expr")
      .def(py::init<PythonBindable>())
      .def("__add__", [](PythonExpr e1,
                         PythonExpr e2) { return PythonExpr(::Add(e1, e2)); })
      .def("__sub__", [](PythonExpr e1,
                         PythonExpr e2) { return PythonExpr(::Sub(e1, e2)); })
      .def("__mul__", [](PythonExpr e1,
                         PythonExpr e2) { return PythonExpr(::Mul(e1, e2)); })
      .def("__div__", [](PythonExpr e1,
                         PythonExpr e2) { return PythonExpr(::Div(e1, e2)); })
      .def("__truediv__",
           [](PythonExpr e1, PythonExpr e2) {
             return PythonExpr(::Div(e1, e2));
           })
      .def("__mod__", [](PythonExpr e1,
                         PythonExpr e2) { return PythonExpr(::Rem(e1, e2)); })
      .def("__lt__", [](PythonExpr e1,
                        PythonExpr e2) { return PythonExpr(::LT(e1, e2)); })
      .def("__le__", [](PythonExpr e1,
                        PythonExpr e2) { return PythonExpr(::LE(e1, e2)); })
      .def("__gt__", [](PythonExpr e1,
                        PythonExpr e2) { return PythonExpr(::GT(e1, e2)); })
      .def("__ge__", [](PythonExpr e1,
                        PythonExpr e2) { return PythonExpr(::GE(e1, e2)); })
      .def("__eq__", [](PythonExpr e1,
                        PythonExpr e2) { return PythonExpr(::EQ(e1, e2)); })
      .def("__ne__", [](PythonExpr e1,
                        PythonExpr e2) { return PythonExpr(::NE(e1, e2)); })
      .def("__and__", [](PythonExpr e1,
                         PythonExpr e2) { return PythonExpr(::And(e1, e2)); })
      .def("__or__", [](PythonExpr e1,
                        PythonExpr e2) { return PythonExpr(::Or(e1, e2)); })
      .def("__invert__", [](PythonExpr e) { return PythonExpr(::Negate(e)); })
      .def("__call__", &dispatchCall)
      .def("__str__", &PythonExpr::str,
           R"DOC(Returns the string value for the Expr)DOC");

  py::class_<PythonBindable>(
      m, "Bindable",
      "Wrapping class for mlir::edsc::Bindable.\nA Bindable is a special Expr "
      "that can be bound manually to specific MLIR SSA Values.")
      .def(py::init<PythonType>())
      .def("__str__", &PythonBindable::str);

  py::class_<PythonStmt>(m, "Stmt", "Wrapping class for mlir::edsc::Stmt.")
      .def(py::init<PythonExpr>())
      .def("__str__", &PythonStmt::str,
           R"DOC(Returns the string value for the Expr)DOC");

  py::class_<PythonIndexed>(
      m, "Indexed",
      "Wrapping class for mlir::edsc::Indexed.\nAn Indexed is a wrapper class "
      "that support load and store operations.")
      .def(py::init<PythonExpr>(), R"DOC(Build from existing Expr)DOC")
      .def(py::init<PythonBindable>(), R"DOC(Build from existing Bindable)DOC")
      .def(
          "load",
          [](PythonIndexed &instance, const py::list &indices) {
            SmallVector<edsc_expr_t, 8> owning;
            return PythonExpr(Load(instance, makeCExprs(owning, indices)));
          },
          R"DOC(Returns an Expr that loads from an Indexed)DOC")
      .def(
          "store",
          [](PythonIndexed &instance, const py::list &indices,
             PythonExpr value) {
            SmallVector<edsc_expr_t, 8> owning;
            return PythonStmt(
                Store(value, instance, makeCExprs(owning, indices)));
          },
          R"DOC(Returns the Stmt that stores into an Indexed)DOC");

  py::class_<PythonMaxExpr>(m, "MaxExpr",
                            "Wrapping class for mlir::edsc::MaxExpr");
  py::class_<PythonMinExpr>(m, "MinExpr",
                            "Wrapping class for mlir::edsc::MinExpr");
}

} // namespace python
} // namespace edsc
} // namespace mlir
