//===- Ops.cpp - Standard MLIR Operations ---------------------------------===//
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

#include "mlir/StandardOps/Ops.h"

#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/IR/Value.h"
#include "mlir/Support/MathExtras.h"
#include "mlir/Support/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"
using namespace mlir;

//===----------------------------------------------------------------------===//
// StandardOpsDialect
//===----------------------------------------------------------------------===//

/// A custom binary operation printer that omits the "std." prefix from the
/// operation names.
void detail::printStandardBinaryOp(Operation *op, OpAsmPrinter *p) {
  assert(op->getNumOperands() == 2 && "binary op should have two operands");
  assert(op->getNumResults() == 1 && "binary op should have one result");

  // If not all the operand and result types are the same, just use the
  // generic assembly form to avoid omitting information in printing.
  auto resultType = op->getResult(0)->getType();
  if (op->getOperand(0)->getType() != resultType ||
      op->getOperand(1)->getType() != resultType) {
    p->printGenericOp(op);
    return;
  }

  *p << op->getName().getStringRef().drop_front(strlen("std.")) << ' '
     << *op->getOperand(0) << ", " << *op->getOperand(1);
  p->printOptionalAttrDict(op->getAttrs());

  // Now we can output only one type for all operands and the result.
  *p << " : " << op->getResult(0)->getType();
}

StandardOpsDialect::StandardOpsDialect(MLIRContext *context)
    : Dialect(/*name=*/"std", context) {
  addOperations<BranchOp, CallOp, CallIndirectOp, CmpFOp, CmpIOp, CondBranchOp,
                DimOp, DmaStartOp, DmaWaitOp, ExtractElementOp, LoadOp,
                MemRefCastOp, ReturnOp, SelectOp, StoreOp, TensorCastOp,
#define GET_OP_LIST
#include "mlir/StandardOps/Ops.cpp.inc"
                >();
}

void mlir::printDimAndSymbolList(Operation::operand_iterator begin,
                                 Operation::operand_iterator end,
                                 unsigned numDims, OpAsmPrinter *p) {
  *p << '(';
  p->printOperands(begin, begin + numDims);
  *p << ')';

  if (begin + numDims != end) {
    *p << '[';
    p->printOperands(begin + numDims, end);
    *p << ']';
  }
}

// Parses dimension and symbol list, and sets 'numDims' to the number of
// dimension operands parsed.
// Returns 'false' on success and 'true' on error.
ParseResult mlir::parseDimAndSymbolList(OpAsmParser *parser,
                                        SmallVector<Value *, 4> &operands,
                                        unsigned &numDims) {
  SmallVector<OpAsmParser::OperandType, 8> opInfos;
  if (parser->parseOperandList(opInfos, -1, OpAsmParser::Delimiter::Paren))
    return failure();
  // Store number of dimensions for validation by caller.
  numDims = opInfos.size();

  // Parse the optional symbol operands.
  auto affineIntTy = parser->getBuilder().getIndexType();
  if (parser->parseOperandList(opInfos, -1,
                               OpAsmParser::Delimiter::OptionalSquare) ||
      parser->resolveOperands(opInfos, affineIntTy, operands))
    return failure();
  return success();
}

/// Matches a ConstantIndexOp.
/// TODO: This should probably just be a general matcher that uses m_Constant
/// and checks the operation for an index type.
static detail::op_matcher<ConstantIndexOp> m_ConstantIndex() {
  return detail::op_matcher<ConstantIndexOp>();
}

//===----------------------------------------------------------------------===//
// Common canonicalization pattern support logic
//===----------------------------------------------------------------------===//

namespace {
/// This is a common class used for patterns of the form
/// "someop(memrefcast) -> someop".  It folds the source of any memref_cast
/// into the root operation directly.
struct MemRefCastFolder : public RewritePattern {
  /// The rootOpName is the name of the root operation to match against.
  MemRefCastFolder(StringRef rootOpName, MLIRContext *context)
      : RewritePattern(rootOpName, 1, context) {}

  PatternMatchResult match(Operation *op) const override {
    for (auto *operand : op->getOperands())
      if (matchPattern(operand, m_Op<MemRefCastOp>()))
        return matchSuccess();

    return matchFailure();
  }

  void rewrite(Operation *op, PatternRewriter &rewriter) const override {
    for (unsigned i = 0, e = op->getNumOperands(); i != e; ++i)
      if (auto *memref = op->getOperand(i)->getDefiningOp())
        if (auto cast = memref->dyn_cast<MemRefCastOp>())
          op->setOperand(i, cast.getOperand());
    rewriter.updatedRootInPlace(op);
  }
};

/// Performs const folding `calculate` with element-wise behavior on the two
/// attributes in `operands` and returns the result if possible.
template <class AttrElementT,
          class ElementValueT = typename AttrElementT::ValueType,
          class CalculationT =
              std::function<ElementValueT(ElementValueT, ElementValueT)>>
Attribute constFoldBinaryOp(ArrayRef<Attribute> operands,
                            const CalculationT &calculate) {
  assert(operands.size() == 2 && "binary op takes two operands");

  if (auto lhs = operands[0].dyn_cast_or_null<AttrElementT>()) {
    auto rhs = operands[1].dyn_cast_or_null<AttrElementT>();
    if (!rhs || lhs.getType() != rhs.getType())
      return {};

    return AttrElementT::get(lhs.getType(),
                             calculate(lhs.getValue(), rhs.getValue()));
  } else if (auto lhs = operands[0].dyn_cast_or_null<SplatElementsAttr>()) {
    auto rhs = operands[1].dyn_cast_or_null<SplatElementsAttr>();
    if (!rhs || lhs.getType() != rhs.getType())
      return {};

    auto elementResult = constFoldBinaryOp<AttrElementT>(
        {lhs.getValue(), rhs.getValue()}, calculate);
    if (!elementResult)
      return {};

    return SplatElementsAttr::get(lhs.getType(), elementResult);
  }
  return {};
}
} // end anonymous namespace.

//===----------------------------------------------------------------------===//
// AddFOp
//===----------------------------------------------------------------------===//

Attribute AddFOp::constantFold(ArrayRef<Attribute> operands,
                               MLIRContext *context) {
  return constFoldBinaryOp<FloatAttr>(
      operands, [](APFloat a, APFloat b) { return a + b; });
}

//===----------------------------------------------------------------------===//
// AddIOp
//===----------------------------------------------------------------------===//

Attribute AddIOp::constantFold(ArrayRef<Attribute> operands,
                               MLIRContext *context) {
  return constFoldBinaryOp<IntegerAttr>(operands,
                                        [](APInt a, APInt b) { return a + b; });
}

Value *AddIOp::fold() {
  /// addi(x, 0) -> x
  if (matchPattern(getOperand(1), m_Zero()))
    return getOperand(0);

  return nullptr;
}

//===----------------------------------------------------------------------===//
// AllocOp
//===----------------------------------------------------------------------===//

static void printAllocOp(OpAsmPrinter *p, AllocOp op) {
  *p << "alloc";

  // Print dynamic dimension operands.
  MemRefType type = op.getType();
  printDimAndSymbolList(op.operand_begin(), op.operand_end(),
                        type.getNumDynamicDims(), p);
  p->printOptionalAttrDict(op.getAttrs(), /*elidedAttrs=*/{"map"});
  *p << " : " << type;
}

static ParseResult parseAllocOp(OpAsmParser *parser, OperationState *result) {
  MemRefType type;

  // Parse the dimension operands and optional symbol operands, followed by a
  // memref type.
  unsigned numDimOperands;
  if (parseDimAndSymbolList(parser, result->operands, numDimOperands) ||
      parser->parseOptionalAttributeDict(result->attributes) ||
      parser->parseColonType(type))
    return failure();

  // Check numDynamicDims against number of question marks in memref type.
  // Note: this check remains here (instead of in verify()), because the
  // partition between dim operands and symbol operands is lost after parsing.
  // Verification still checks that the total number of operands matches
  // the number of symbols in the affine map, plus the number of dynamic
  // dimensions in the memref.
  if (numDimOperands != type.getNumDynamicDims())
    return parser->emitError(parser->getNameLoc())
           << "dimension operand count does not equal memref dynamic dimension "
              "count";
  result->types.push_back(type);
  return success();
}

static LogicalResult verify(AllocOp op) {
  auto memRefType = op.getResult()->getType().dyn_cast<MemRefType>();
  if (!memRefType)
    return op.emitOpError("result must be a memref");

  unsigned numSymbols = 0;
  if (!memRefType.getAffineMaps().empty()) {
    AffineMap affineMap = memRefType.getAffineMaps()[0];
    // Store number of symbols used in affine map (used in subsequent check).
    numSymbols = affineMap.getNumSymbols();
    // TODO(zinenko): this check does not belong to AllocOp, or any other op but
    // to the type system itself.  It has been partially hoisted to Parser but
    // remains here in case an AllocOp gets constructed programmatically.
    // Remove when we can emit errors directly from *Type::get(...) functions.
    //
    // Verify that the layout affine map matches the rank of the memref.
    if (affineMap.getNumDims() != memRefType.getRank())
      return op.emitOpError(
          "affine map dimension count must equal memref rank");
  }
  unsigned numDynamicDims = memRefType.getNumDynamicDims();
  // Check that the total number of operands matches the number of symbols in
  // the affine map, plus the number of dynamic dimensions specified in the
  // memref type.
  if (op.getOperation()->getNumOperands() != numDynamicDims + numSymbols)
    return op.emitOpError(
        "operand count does not equal dimension plus symbol operand count");

  // Verify that all operands are of type Index.
  for (auto *operand : op.getOperands())
    if (!operand->getType().isIndex())
      return op.emitOpError("requires operands to be of type Index");
  return success();
}

namespace {
/// Fold constant dimensions into an alloc operation.
struct SimplifyAllocConst : public RewritePattern {
  SimplifyAllocConst(MLIRContext *context)
      : RewritePattern(AllocOp::getOperationName(), 1, context) {}

  PatternMatchResult match(Operation *op) const override {
    auto alloc = op->cast<AllocOp>();

    // Check to see if any dimensions operands are constants.  If so, we can
    // substitute and drop them.
    for (auto *operand : alloc.getOperands())
      if (matchPattern(operand, m_ConstantIndex()))
        return matchSuccess();
    return matchFailure();
  }

  void rewrite(Operation *op, PatternRewriter &rewriter) const override {
    auto allocOp = op->cast<AllocOp>();
    auto memrefType = allocOp.getType();

    // Ok, we have one or more constant operands.  Collect the non-constant ones
    // and keep track of the resultant memref type to build.
    SmallVector<int64_t, 4> newShapeConstants;
    newShapeConstants.reserve(memrefType.getRank());
    SmallVector<Value *, 4> newOperands;
    SmallVector<Value *, 4> droppedOperands;

    unsigned dynamicDimPos = 0;
    for (unsigned dim = 0, e = memrefType.getRank(); dim < e; ++dim) {
      int64_t dimSize = memrefType.getDimSize(dim);
      // If this is already static dimension, keep it.
      if (dimSize != -1) {
        newShapeConstants.push_back(dimSize);
        continue;
      }
      auto *defOp = allocOp.getOperand(dynamicDimPos)->getDefiningOp();
      if (auto constantIndexOp = dyn_cast_or_null<ConstantIndexOp>(defOp)) {
        // Dynamic shape dimension will be folded.
        newShapeConstants.push_back(constantIndexOp.getValue());
        // Record to check for zero uses later below.
        droppedOperands.push_back(constantIndexOp);
      } else {
        // Dynamic shape dimension not folded; copy operand from old memref.
        newShapeConstants.push_back(-1);
        newOperands.push_back(allocOp.getOperand(dynamicDimPos));
      }
      dynamicDimPos++;
    }

    // Create new memref type (which will have fewer dynamic dimensions).
    auto newMemRefType = MemRefType::get(
        newShapeConstants, memrefType.getElementType(),
        memrefType.getAffineMaps(), memrefType.getMemorySpace());
    assert(newOperands.size() == newMemRefType.getNumDynamicDims());

    // Create and insert the alloc op for the new memref.
    auto newAlloc =
        rewriter.create<AllocOp>(allocOp.getLoc(), newMemRefType, newOperands);
    // Insert a cast so we have the same type as the old alloc.
    auto resultCast = rewriter.create<MemRefCastOp>(allocOp.getLoc(), newAlloc,
                                                    allocOp.getType());

    rewriter.replaceOp(op, {resultCast}, droppedOperands);
  }
};

/// Fold alloc operations with no uses. Alloc has side effects on the heap,
/// but can still be deleted if it has zero uses.
struct SimplifyDeadAlloc : public RewritePattern {
  SimplifyDeadAlloc(MLIRContext *context)
      : RewritePattern(AllocOp::getOperationName(), 1, context) {}

  PatternMatchResult matchAndRewrite(Operation *op,
                                     PatternRewriter &rewriter) const override {
    // Check if the alloc'ed value has any uses.
    auto alloc = op->cast<AllocOp>();
    if (!alloc.use_empty())
      return matchFailure();

    // If it doesn't, we can eliminate it.
    op->erase();
    return matchSuccess();
  }
};
} // end anonymous namespace.

void AllocOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context) {
  results.push_back(llvm::make_unique<SimplifyAllocConst>(context));
  results.push_back(llvm::make_unique<SimplifyDeadAlloc>(context));
}

//===----------------------------------------------------------------------===//
// BranchOp
//===----------------------------------------------------------------------===//

void BranchOp::build(Builder *builder, OperationState *result, Block *dest,
                     ArrayRef<Value *> operands) {
  result->addSuccessor(dest, operands);
}

ParseResult BranchOp::parse(OpAsmParser *parser, OperationState *result) {
  Block *dest;
  SmallVector<Value *, 4> destOperands;
  if (parser->parseSuccessorAndUseList(dest, destOperands))
    return failure();
  result->addSuccessor(dest, destOperands);
  return success();
}

void BranchOp::print(OpAsmPrinter *p) {
  *p << "br ";
  p->printSuccessorAndUseList(getOperation(), 0);
}

Block *BranchOp::getDest() { return getOperation()->getSuccessor(0); }

void BranchOp::setDest(Block *block) {
  return getOperation()->setSuccessor(block, 0);
}

void BranchOp::eraseOperand(unsigned index) {
  getOperation()->eraseSuccessorOperand(0, index);
}

//===----------------------------------------------------------------------===//
// CallOp
//===----------------------------------------------------------------------===//

void CallOp::build(Builder *builder, OperationState *result, Function *callee,
                   ArrayRef<Value *> operands) {
  result->addOperands(operands);
  result->addAttribute("callee", builder->getFunctionAttr(callee));
  result->addTypes(callee->getType().getResults());
}

ParseResult CallOp::parse(OpAsmParser *parser, OperationState *result) {
  StringRef calleeName;
  llvm::SMLoc calleeLoc;
  FunctionType calleeType;
  SmallVector<OpAsmParser::OperandType, 4> operands;
  Function *callee = nullptr;
  if (parser->parseFunctionName(calleeName, calleeLoc) ||
      parser->parseOperandList(operands, /*requiredOperandCount=*/-1,
                               OpAsmParser::Delimiter::Paren) ||
      parser->parseOptionalAttributeDict(result->attributes) ||
      parser->parseColonType(calleeType) ||
      parser->resolveFunctionName(calleeName, calleeType, calleeLoc, callee) ||
      parser->addTypesToList(calleeType.getResults(), result->types) ||
      parser->resolveOperands(operands, calleeType.getInputs(), calleeLoc,
                              result->operands))
    return failure();

  result->addAttribute("callee", parser->getBuilder().getFunctionAttr(callee));
  return success();
}

void CallOp::print(OpAsmPrinter *p) {
  *p << "call ";
  p->printFunctionReference(getCallee());
  *p << '(';
  p->printOperands(getOperands());
  *p << ')';
  p->printOptionalAttrDict(getAttrs(), /*elidedAttrs=*/{"callee"});
  *p << " : " << getCallee()->getType();
}

LogicalResult CallOp::verify() {
  // Check that the callee attribute was specified.
  auto fnAttr = getAttrOfType<FunctionAttr>("callee");
  if (!fnAttr)
    return emitOpError("requires a 'callee' function attribute");

  // Verify that the operand and result types match the callee.
  auto fnType = fnAttr.getValue()->getType();
  if (fnType.getNumInputs() != getNumOperands())
    return emitOpError("incorrect number of operands for callee");

  for (unsigned i = 0, e = fnType.getNumInputs(); i != e; ++i) {
    if (getOperand(i)->getType() != fnType.getInput(i))
      return emitOpError("operand type mismatch");
  }

  if (fnType.getNumResults() != getNumResults())
    return emitOpError("incorrect number of results for callee");

  for (unsigned i = 0, e = fnType.getNumResults(); i != e; ++i) {
    if (getResult(i)->getType() != fnType.getResult(i))
      return emitOpError("result type mismatch");
  }

  return success();
}

//===----------------------------------------------------------------------===//
// CallIndirectOp
//===----------------------------------------------------------------------===//
namespace {
/// Fold indirect calls that have a constant function as the callee operand.
struct SimplifyIndirectCallWithKnownCallee : public RewritePattern {
  SimplifyIndirectCallWithKnownCallee(MLIRContext *context)
      : RewritePattern(CallIndirectOp::getOperationName(), 1, context) {}

  PatternMatchResult matchAndRewrite(Operation *op,
                                     PatternRewriter &rewriter) const override {
    auto indirectCall = op->cast<CallIndirectOp>();

    // Check that the callee is a constant operation.
    Attribute callee;
    if (!matchPattern(indirectCall.getCallee(), m_Constant(&callee)))
      return matchFailure();

    // Check that the constant callee is a function.
    FunctionAttr calledFn = callee.dyn_cast<FunctionAttr>();
    if (!calledFn)
      return matchFailure();

    // Replace with a direct call.
    SmallVector<Value *, 8> callOperands(indirectCall.getArgOperands());
    rewriter.replaceOpWithNewOp<CallOp>(op, calledFn.getValue(), callOperands);
    return matchSuccess();
  }
};
} // end anonymous namespace.

void CallIndirectOp::build(Builder *builder, OperationState *result,
                           Value *callee, ArrayRef<Value *> operands) {
  auto fnType = callee->getType().cast<FunctionType>();
  result->operands.push_back(callee);
  result->addOperands(operands);
  result->addTypes(fnType.getResults());
}

ParseResult CallIndirectOp::parse(OpAsmParser *parser, OperationState *result) {
  FunctionType calleeType;
  OpAsmParser::OperandType callee;
  llvm::SMLoc operandsLoc;
  SmallVector<OpAsmParser::OperandType, 4> operands;
  return failure(
      parser->parseOperand(callee) ||
      parser->getCurrentLocation(&operandsLoc) ||
      parser->parseOperandList(operands, /*requiredOperandCount=*/-1,
                               OpAsmParser::Delimiter::Paren) ||
      parser->parseOptionalAttributeDict(result->attributes) ||
      parser->parseColonType(calleeType) ||
      parser->resolveOperand(callee, calleeType, result->operands) ||
      parser->resolveOperands(operands, calleeType.getInputs(), operandsLoc,
                              result->operands) ||
      parser->addTypesToList(calleeType.getResults(), result->types));
}

void CallIndirectOp::print(OpAsmPrinter *p) {
  *p << "call_indirect ";
  p->printOperand(getCallee());
  *p << '(';
  auto operandRange = getOperands();
  p->printOperands(++operandRange.begin(), operandRange.end());
  *p << ')';
  p->printOptionalAttrDict(getAttrs(), /*elidedAttrs=*/{"callee"});
  *p << " : " << getCallee()->getType();
}

LogicalResult CallIndirectOp::verify() {
  // The callee must be a function.
  auto fnType = getCallee()->getType().dyn_cast<FunctionType>();
  if (!fnType)
    return emitOpError("callee must have function type");

  // Verify that the operand and result types match the callee.
  if (fnType.getNumInputs() != getNumOperands() - 1)
    return emitOpError("incorrect number of operands for callee");

  for (unsigned i = 0, e = fnType.getNumInputs(); i != e; ++i) {
    if (getOperand(i + 1)->getType() != fnType.getInput(i))
      return emitOpError("operand type mismatch");
  }

  if (fnType.getNumResults() != getNumResults())
    return emitOpError("incorrect number of results for callee");

  for (unsigned i = 0, e = fnType.getNumResults(); i != e; ++i) {
    if (getResult(i)->getType() != fnType.getResult(i))
      return emitOpError("result type mismatch");
  }

  return success();
}

void CallIndirectOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.push_back(
      llvm::make_unique<SimplifyIndirectCallWithKnownCallee>(context));
}

//===----------------------------------------------------------------------===//
// General helpers for comparison ops
//===----------------------------------------------------------------------===//

// Return the type of the same shape (scalar, vector or tensor) containing i1.
static Type getCheckedI1SameShape(Builder *build, Type type) {
  auto i1Type = build->getI1Type();
  if (type.isIntOrIndexOrFloat())
    return i1Type;
  if (auto tensorType = type.dyn_cast<RankedTensorType>())
    return build->getTensorType(tensorType.getShape(), i1Type);
  if (auto tensorType = type.dyn_cast<UnrankedTensorType>())
    return build->getTensorType(i1Type);
  if (auto vectorType = type.dyn_cast<VectorType>())
    return build->getVectorType(vectorType.getShape(), i1Type);
  return Type();
}

static Type getI1SameShape(Builder *build, Type type) {
  Type res = getCheckedI1SameShape(build, type);
  assert(res && "expected type with valid i1 shape");
  return res;
}

static inline bool isI1(Type type) {
  return type.isa<IntegerType>() && type.cast<IntegerType>().getWidth() == 1;
}

template <typename Ty>
static inline bool implCheckI1SameShape(Ty pattern, Type type) {
  auto specificType = type.dyn_cast<Ty>();
  if (!specificType)
    return true;
  if (specificType.getShape() != pattern.getShape())
    return true;
  return !isI1(specificType.getElementType());
}

// Checks if "type" has the same shape (scalar, vector or tensor) as "pattern"
// and contains i1.
static bool checkI1SameShape(Type pattern, Type type) {
  if (pattern.isIntOrIndexOrFloat())
    return !isI1(type);
  if (auto patternTensorType = pattern.dyn_cast<TensorType>())
    return implCheckI1SameShape(patternTensorType, type);
  if (auto patternVectorType = pattern.dyn_cast<VectorType>())
    return implCheckI1SameShape(patternVectorType, type);

  llvm_unreachable("unsupported type");
}

//===----------------------------------------------------------------------===//
// CmpIOp
//===----------------------------------------------------------------------===//

// Returns an array of mnemonics for CmpIPredicates indexed by values thereof.
static inline const char *const *getCmpIPredicateNames() {
  static const char *predicateNames[]{
      /*EQ*/ "eq",
      /*NE*/ "ne",
      /*SLT*/ "slt",
      /*SLE*/ "sle",
      /*SGT*/ "sgt",
      /*SGE*/ "sge",
      /*ULT*/ "ult",
      /*ULE*/ "ule",
      /*UGT*/ "ugt",
      /*UGE*/ "uge",
  };
  static_assert(std::extent<decltype(predicateNames)>::value ==
                    (size_t)CmpIPredicate::NumPredicates,
                "wrong number of predicate names");
  return predicateNames;
}

// Returns a value of the predicate corresponding to the given mnemonic.
// Returns NumPredicates (one-past-end) if there is no such mnemonic.
CmpIPredicate CmpIOp::getPredicateByName(StringRef name) {
  return llvm::StringSwitch<CmpIPredicate>(name)
      .Case("eq", CmpIPredicate::EQ)
      .Case("ne", CmpIPredicate::NE)
      .Case("slt", CmpIPredicate::SLT)
      .Case("sle", CmpIPredicate::SLE)
      .Case("sgt", CmpIPredicate::SGT)
      .Case("sge", CmpIPredicate::SGE)
      .Case("ult", CmpIPredicate::ULT)
      .Case("ule", CmpIPredicate::ULE)
      .Case("ugt", CmpIPredicate::UGT)
      .Case("uge", CmpIPredicate::UGE)
      .Default(CmpIPredicate::NumPredicates);
}

void CmpIOp::build(Builder *build, OperationState *result,
                   CmpIPredicate predicate, Value *lhs, Value *rhs) {
  result->addOperands({lhs, rhs});
  result->types.push_back(getI1SameShape(build, lhs->getType()));
  result->addAttribute(
      getPredicateAttrName(),
      build->getI64IntegerAttr(static_cast<int64_t>(predicate)));
}

ParseResult CmpIOp::parse(OpAsmParser *parser, OperationState *result) {
  SmallVector<OpAsmParser::OperandType, 2> ops;
  SmallVector<NamedAttribute, 4> attrs;
  Attribute predicateNameAttr;
  Type type;
  if (parser->parseAttribute(predicateNameAttr, getPredicateAttrName(),
                             attrs) ||
      parser->parseComma() || parser->parseOperandList(ops, 2) ||
      parser->parseOptionalAttributeDict(attrs) ||
      parser->parseColonType(type) ||
      parser->resolveOperands(ops, type, result->operands))
    return failure();

  if (!predicateNameAttr.isa<StringAttr>())
    return parser->emitError(parser->getNameLoc(),
                             "expected string comparison predicate attribute");

  // Rewrite string attribute to an enum value.
  StringRef predicateName = predicateNameAttr.cast<StringAttr>().getValue();
  auto predicate = getPredicateByName(predicateName);
  if (predicate == CmpIPredicate::NumPredicates)
    return parser->emitError(parser->getNameLoc(),
                             "unknown comparison predicate \"" + predicateName +
                                 "\"");

  auto builder = parser->getBuilder();
  Type i1Type = getCheckedI1SameShape(&builder, type);
  if (!i1Type)
    return parser->emitError(parser->getNameLoc(),
                             "expected type with valid i1 shape");

  attrs[0].second = builder.getI64IntegerAttr(static_cast<int64_t>(predicate));
  result->attributes = attrs;

  result->addTypes({i1Type});
  return success();
}

void CmpIOp::print(OpAsmPrinter *p) {
  *p << "cmpi ";

  auto predicateValue =
      getAttrOfType<IntegerAttr>(getPredicateAttrName()).getInt();
  assert(predicateValue >= static_cast<int>(CmpIPredicate::FirstValidValue) &&
         predicateValue < static_cast<int>(CmpIPredicate::NumPredicates) &&
         "unknown predicate index");
  Builder b(getContext());
  auto predicateStringAttr =
      b.getStringAttr(getCmpIPredicateNames()[predicateValue]);
  p->printAttribute(predicateStringAttr);

  *p << ", ";
  p->printOperand(getOperand(0));
  *p << ", ";
  p->printOperand(getOperand(1));
  p->printOptionalAttrDict(getAttrs(),
                           /*elidedAttrs=*/{getPredicateAttrName()});
  *p << " : " << getOperand(0)->getType();
}

LogicalResult CmpIOp::verify() {
  auto predicateAttr = getAttrOfType<IntegerAttr>(getPredicateAttrName());
  if (!predicateAttr)
    return emitOpError("requires an integer attribute named 'predicate'");
  auto predicate = predicateAttr.getInt();
  if (predicate < (int64_t)CmpIPredicate::FirstValidValue ||
      predicate >= (int64_t)CmpIPredicate::NumPredicates)
    return emitOpError("'predicate' attribute value out of range");

  return success();
}

// Compute `lhs` `pred` `rhs`, where `pred` is one of the known integer
// comparison predicates.
static bool applyCmpPredicate(CmpIPredicate predicate, const APInt &lhs,
                              const APInt &rhs) {
  switch (predicate) {
  case CmpIPredicate::EQ:
    return lhs.eq(rhs);
  case CmpIPredicate::NE:
    return lhs.ne(rhs);
  case CmpIPredicate::SLT:
    return lhs.slt(rhs);
  case CmpIPredicate::SLE:
    return lhs.sle(rhs);
  case CmpIPredicate::SGT:
    return lhs.sgt(rhs);
  case CmpIPredicate::SGE:
    return lhs.sge(rhs);
  case CmpIPredicate::ULT:
    return lhs.ult(rhs);
  case CmpIPredicate::ULE:
    return lhs.ule(rhs);
  case CmpIPredicate::UGT:
    return lhs.ugt(rhs);
  case CmpIPredicate::UGE:
    return lhs.uge(rhs);
  default:
    llvm_unreachable("unknown comparison predicate");
  }
}

// Constant folding hook for comparisons.
Attribute CmpIOp::constantFold(ArrayRef<Attribute> operands,
                               MLIRContext *context) {
  assert(operands.size() == 2 && "cmpi takes two arguments");

  auto lhs = operands.front().dyn_cast_or_null<IntegerAttr>();
  auto rhs = operands.back().dyn_cast_or_null<IntegerAttr>();
  if (!lhs || !rhs)
    return {};

  auto val = applyCmpPredicate(getPredicate(), lhs.getValue(), rhs.getValue());
  return IntegerAttr::get(IntegerType::get(1, context), APInt(1, val));
}

//===----------------------------------------------------------------------===//
// CmpFOp
//===----------------------------------------------------------------------===//

// Returns an array of mnemonics for CmpFPredicates indexed by values thereof.
static inline const char *const *getCmpFPredicateNames() {
  static const char *predicateNames[] = {
      /*FALSE*/ "false",
      /*OEQ*/ "oeq",
      /*OGT*/ "ogt",
      /*OGE*/ "oge",
      /*OLT*/ "olt",
      /*OLE*/ "ole",
      /*ONE*/ "one",
      /*ORD*/ "ord",
      /*UEQ*/ "ueq",
      /*UGT*/ "ugt",
      /*UGE*/ "uge",
      /*ULT*/ "ult",
      /*ULE*/ "ule",
      /*UNE*/ "une",
      /*UNO*/ "uno",
      /*TRUE*/ "true",
  };
  static_assert(std::extent<decltype(predicateNames)>::value ==
                    (size_t)CmpFPredicate::NumPredicates,
                "wrong number of predicate names");
  return predicateNames;
}

// Returns a value of the predicate corresponding to the given mnemonic.
// Returns NumPredicates (one-past-end) if there is no such mnemonic.
CmpFPredicate CmpFOp::getPredicateByName(StringRef name) {
  return llvm::StringSwitch<CmpFPredicate>(name)
      .Case("false", CmpFPredicate::FALSE)
      .Case("oeq", CmpFPredicate::OEQ)
      .Case("ogt", CmpFPredicate::OGT)
      .Case("oge", CmpFPredicate::OGE)
      .Case("olt", CmpFPredicate::OLT)
      .Case("ole", CmpFPredicate::OLE)
      .Case("one", CmpFPredicate::ONE)
      .Case("ord", CmpFPredicate::ORD)
      .Case("ueq", CmpFPredicate::UEQ)
      .Case("ugt", CmpFPredicate::UGT)
      .Case("uge", CmpFPredicate::UGE)
      .Case("ult", CmpFPredicate::ULT)
      .Case("ule", CmpFPredicate::ULE)
      .Case("une", CmpFPredicate::UNE)
      .Case("uno", CmpFPredicate::UNO)
      .Case("true", CmpFPredicate::TRUE)
      .Default(CmpFPredicate::NumPredicates);
}

void CmpFOp::build(Builder *build, OperationState *result,
                   CmpFPredicate predicate, Value *lhs, Value *rhs) {
  result->addOperands({lhs, rhs});
  result->types.push_back(getI1SameShape(build, lhs->getType()));
  result->addAttribute(
      getPredicateAttrName(),
      build->getI64IntegerAttr(static_cast<int64_t>(predicate)));
}

ParseResult CmpFOp::parse(OpAsmParser *parser, OperationState *result) {
  SmallVector<OpAsmParser::OperandType, 2> ops;
  SmallVector<NamedAttribute, 4> attrs;
  Attribute predicateNameAttr;
  Type type;
  if (parser->parseAttribute(predicateNameAttr, getPredicateAttrName(),
                             attrs) ||
      parser->parseComma() || parser->parseOperandList(ops, 2) ||
      parser->parseOptionalAttributeDict(attrs) ||
      parser->parseColonType(type) ||
      parser->resolveOperands(ops, type, result->operands))
    return failure();

  if (!predicateNameAttr.isa<StringAttr>())
    return parser->emitError(parser->getNameLoc(),
                             "expected string comparison predicate attribute");

  // Rewrite string attribute to an enum value.
  StringRef predicateName = predicateNameAttr.cast<StringAttr>().getValue();
  auto predicate = getPredicateByName(predicateName);
  if (predicate == CmpFPredicate::NumPredicates)
    return parser->emitError(parser->getNameLoc(),
                             "unknown comparison predicate \"" + predicateName +
                                 "\"");

  auto builder = parser->getBuilder();
  Type i1Type = getCheckedI1SameShape(&builder, type);
  if (!i1Type)
    return parser->emitError(parser->getNameLoc(),
                             "expected type with valid i1 shape");

  attrs[0].second = builder.getI64IntegerAttr(static_cast<int64_t>(predicate));
  result->attributes = attrs;

  result->addTypes({i1Type});
  return success();
}

void CmpFOp::print(OpAsmPrinter *p) {
  *p << "cmpf ";

  auto predicateValue =
      getAttrOfType<IntegerAttr>(getPredicateAttrName()).getInt();
  assert(predicateValue >= static_cast<int>(CmpFPredicate::FirstValidValue) &&
         predicateValue < static_cast<int>(CmpFPredicate::NumPredicates) &&
         "unknown predicate index");
  Builder b(getContext());
  auto predicateStringAttr =
      b.getStringAttr(getCmpFPredicateNames()[predicateValue]);
  p->printAttribute(predicateStringAttr);

  *p << ", ";
  p->printOperand(getOperand(0));
  *p << ", ";
  p->printOperand(getOperand(1));
  p->printOptionalAttrDict(getAttrs(),
                           /*elidedAttrs=*/{getPredicateAttrName()});
  *p << " : " << getOperand(0)->getType();
}

LogicalResult CmpFOp::verify() {
  auto predicateAttr = getAttrOfType<IntegerAttr>(getPredicateAttrName());
  if (!predicateAttr)
    return emitOpError("requires an integer attribute named 'predicate'");
  auto predicate = predicateAttr.getInt();
  if (predicate < (int64_t)CmpFPredicate::FirstValidValue ||
      predicate >= (int64_t)CmpFPredicate::NumPredicates)
    return emitOpError("'predicate' attribute value out of range");

  return success();
}

// Compute `lhs` `pred` `rhs`, where `pred` is one of the known floating point
// comparison predicates.
static bool applyCmpPredicate(CmpFPredicate predicate, const APFloat &lhs,
                              const APFloat &rhs) {
  auto cmpResult = lhs.compare(rhs);
  switch (predicate) {
  case CmpFPredicate::FALSE:
    return false;
  case CmpFPredicate::OEQ:
    return cmpResult == APFloat::cmpEqual;
  case CmpFPredicate::OGT:
    return cmpResult == APFloat::cmpGreaterThan;
  case CmpFPredicate::OGE:
    return cmpResult == APFloat::cmpGreaterThan ||
           cmpResult == APFloat::cmpEqual;
  case CmpFPredicate::OLT:
    return cmpResult == APFloat::cmpLessThan;
  case CmpFPredicate::OLE:
    return cmpResult == APFloat::cmpLessThan || cmpResult == APFloat::cmpEqual;
  case CmpFPredicate::ONE:
    return cmpResult != APFloat::cmpUnordered && cmpResult != APFloat::cmpEqual;
  case CmpFPredicate::ORD:
    return cmpResult != APFloat::cmpUnordered;
  case CmpFPredicate::UEQ:
    return cmpResult == APFloat::cmpUnordered || cmpResult == APFloat::cmpEqual;
  case CmpFPredicate::UGT:
    return cmpResult == APFloat::cmpUnordered ||
           cmpResult == APFloat::cmpGreaterThan;
  case CmpFPredicate::UGE:
    return cmpResult == APFloat::cmpUnordered ||
           cmpResult == APFloat::cmpGreaterThan ||
           cmpResult == APFloat::cmpEqual;
  case CmpFPredicate::ULT:
    return cmpResult == APFloat::cmpUnordered ||
           cmpResult == APFloat::cmpLessThan;
  case CmpFPredicate::ULE:
    return cmpResult == APFloat::cmpUnordered ||
           cmpResult == APFloat::cmpLessThan || cmpResult == APFloat::cmpEqual;
  case CmpFPredicate::UNE:
    return cmpResult != APFloat::cmpEqual;
  case CmpFPredicate::UNO:
    return cmpResult == APFloat::cmpUnordered;
  case CmpFPredicate::TRUE:
    return true;
  default:
    llvm_unreachable("unknown comparison predicate");
  }
}

// Constant folding hook for comparisons.
Attribute CmpFOp::constantFold(ArrayRef<Attribute> operands,
                               MLIRContext *context) {
  assert(operands.size() == 2 && "cmpf takes two arguments");

  auto lhs = operands.front().dyn_cast_or_null<FloatAttr>();
  auto rhs = operands.back().dyn_cast_or_null<FloatAttr>();
  if (!lhs || !rhs ||
      // TODO(b/122019992) Implement and test constant folding for nan/inf when
      // it is possible to have constant nan/inf
      !lhs.getValue().isFinite() || !rhs.getValue().isFinite())
    return {};

  auto val = applyCmpPredicate(getPredicate(), lhs.getValue(), rhs.getValue());
  return IntegerAttr::get(IntegerType::get(1, context), APInt(1, val));
}

//===----------------------------------------------------------------------===//
// CondBranchOp
//===----------------------------------------------------------------------===//

namespace {
/// cond_br true, ^bb1, ^bb2 -> br ^bb1
/// cond_br false, ^bb1, ^bb2 -> br ^bb2
///
struct SimplifyConstCondBranchPred : public RewritePattern {
  SimplifyConstCondBranchPred(MLIRContext *context)
      : RewritePattern(CondBranchOp::getOperationName(), 1, context) {}

  PatternMatchResult matchAndRewrite(Operation *op,
                                     PatternRewriter &rewriter) const override {
    auto condbr = op->cast<CondBranchOp>();

    // Check that the condition is a constant.
    if (!matchPattern(condbr.getCondition(), m_Op<ConstantOp>()))
      return matchFailure();

    Block *foldedDest;
    SmallVector<Value *, 4> branchArgs;

    // If the condition is known to evaluate to false we fold to a branch to the
    // false destination. Otherwise, we fold to a branch to the true
    // destination.
    if (matchPattern(condbr.getCondition(), m_Zero())) {
      foldedDest = condbr.getFalseDest();
      branchArgs.assign(condbr.false_operand_begin(),
                        condbr.false_operand_end());
    } else {
      foldedDest = condbr.getTrueDest();
      branchArgs.assign(condbr.true_operand_begin(), condbr.true_operand_end());
    }

    rewriter.replaceOpWithNewOp<BranchOp>(op, foldedDest, branchArgs);
    return matchSuccess();
  }
};
} // end anonymous namespace.

void CondBranchOp::build(Builder *builder, OperationState *result,
                         Value *condition, Block *trueDest,
                         ArrayRef<Value *> trueOperands, Block *falseDest,
                         ArrayRef<Value *> falseOperands) {
  result->addOperands(condition);
  result->addSuccessor(trueDest, trueOperands);
  result->addSuccessor(falseDest, falseOperands);
}

ParseResult CondBranchOp::parse(OpAsmParser *parser, OperationState *result) {
  SmallVector<Value *, 4> destOperands;
  Block *dest;
  OpAsmParser::OperandType condInfo;

  // Parse the condition.
  Type int1Ty = parser->getBuilder().getI1Type();
  if (parser->parseOperand(condInfo) || parser->parseComma() ||
      parser->resolveOperand(condInfo, int1Ty, result->operands)) {
    return parser->emitError(parser->getNameLoc(),
                             "expected condition type was boolean (i1)");
  }

  // Parse the true successor.
  if (parser->parseSuccessorAndUseList(dest, destOperands))
    return failure();
  result->addSuccessor(dest, destOperands);

  // Parse the false successor.
  destOperands.clear();
  if (parser->parseComma() ||
      parser->parseSuccessorAndUseList(dest, destOperands))
    return failure();
  result->addSuccessor(dest, destOperands);

  return success();
}

void CondBranchOp::print(OpAsmPrinter *p) {
  *p << "cond_br ";
  p->printOperand(getCondition());
  *p << ", ";
  p->printSuccessorAndUseList(getOperation(), trueIndex);
  *p << ", ";
  p->printSuccessorAndUseList(getOperation(), falseIndex);
}

LogicalResult CondBranchOp::verify() {
  if (!getCondition()->getType().isInteger(1))
    return emitOpError("expected condition type was boolean (i1)");
  return success();
}

void CondBranchOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.push_back(llvm::make_unique<SimplifyConstCondBranchPred>(context));
}

Block *CondBranchOp::getTrueDest() {
  return getOperation()->getSuccessor(trueIndex);
}

Block *CondBranchOp::getFalseDest() {
  return getOperation()->getSuccessor(falseIndex);
}

unsigned CondBranchOp::getNumTrueOperands() {
  return getOperation()->getNumSuccessorOperands(trueIndex);
}

void CondBranchOp::eraseTrueOperand(unsigned index) {
  getOperation()->eraseSuccessorOperand(trueIndex, index);
}

unsigned CondBranchOp::getNumFalseOperands() {
  return getOperation()->getNumSuccessorOperands(falseIndex);
}

void CondBranchOp::eraseFalseOperand(unsigned index) {
  getOperation()->eraseSuccessorOperand(falseIndex, index);
}

//===----------------------------------------------------------------------===//
// Constant*Op
//===----------------------------------------------------------------------===//

static void printConstantOp(OpAsmPrinter *p, ConstantOp &op) {
  *p << "constant ";
  p->printOptionalAttrDict(op.getAttrs(), /*elidedAttrs=*/{"value"});

  if (op.getAttrs().size() > 1)
    *p << ' ';
  p->printAttributeAndType(op.getValue());
}

static ParseResult parseConstantOp(OpAsmParser *parser,
                                   OperationState *result) {
  Attribute valueAttr;
  Type type;

  if (parser->parseOptionalAttributeDict(result->attributes) ||
      parser->parseAttribute(valueAttr, "value", result->attributes))
    return failure();

  // Add the attribute type to the list.
  return parser->addTypeToList(valueAttr.getType(), result->types);
}

/// The constant op requires an attribute, and furthermore requires that it
/// matches the return type.
static LogicalResult verify(ConstantOp &op) {
  auto value = op.getValue();
  if (!value)
    return op.emitOpError("requires a 'value' attribute");

  auto type = op.getType();
  if (type != value.getType())
    return op.emitOpError() << "requires attribute's type (" << value.getType()
                            << ") to match op's return type (" << type << ")";

  if (type.isa<IndexType>())
    return success();

  if (auto intAttr = value.dyn_cast<IntegerAttr>()) {
    // If the type has a known bitwidth we verify that the value can be
    // represented with the given bitwidth.
    auto bitwidth = type.cast<IntegerType>().getWidth();
    auto intVal = intAttr.getValue();
    if (!intVal.isSignedIntN(bitwidth) && !intVal.isIntN(bitwidth))
      return op.emitOpError("requires 'value' to be an integer within the "
                            "range of the integer result type");
    return success();
  }

  if (type.isa<FloatType>()) {
    if (!value.isa<FloatAttr>())
      return op.emitOpError("requires 'value' to be a floating point constant");
    return success();
  }

  if (type.isa<VectorOrTensorType>()) {
    if (!value.isa<ElementsAttr>())
      return op.emitOpError("requires 'value' to be a vector/tensor constant");
    return success();
  }

  if (type.isa<FunctionType>()) {
    if (!value.isa<FunctionAttr>())
      return op.emitOpError("requires 'value' to be a function reference");
    return success();
  }

  return op.emitOpError(
      "requires a result type that aligns with the 'value' attribute");
}

Attribute ConstantOp::constantFold(ArrayRef<Attribute> operands,
                                   MLIRContext *context) {
  assert(operands.empty() && "constant has no operands");
  return getValue();
}

void ConstantFloatOp::build(Builder *builder, OperationState *result,
                            const APFloat &value, FloatType type) {
  ConstantOp::build(builder, result, type, builder->getFloatAttr(type, value));
}

bool ConstantFloatOp::isClassFor(Operation *op) {
  return ConstantOp::isClassFor(op) &&
         op->getResult(0)->getType().isa<FloatType>();
}

/// ConstantIntOp only matches values whose result type is an IntegerType.
bool ConstantIntOp::isClassFor(Operation *op) {
  return ConstantOp::isClassFor(op) &&
         op->getResult(0)->getType().isa<IntegerType>();
}

void ConstantIntOp::build(Builder *builder, OperationState *result,
                          int64_t value, unsigned width) {
  Type type = builder->getIntegerType(width);
  ConstantOp::build(builder, result, type,
                    builder->getIntegerAttr(type, value));
}

/// Build a constant int op producing an integer with the specified type,
/// which must be an integer type.
void ConstantIntOp::build(Builder *builder, OperationState *result,
                          int64_t value, Type type) {
  assert(type.isa<IntegerType>() && "ConstantIntOp can only have integer type");
  ConstantOp::build(builder, result, type,
                    builder->getIntegerAttr(type, value));
}

/// ConstantIndexOp only matches values whose result type is Index.
bool ConstantIndexOp::isClassFor(Operation *op) {
  return ConstantOp::isClassFor(op) && op->getResult(0)->getType().isIndex();
}

void ConstantIndexOp::build(Builder *builder, OperationState *result,
                            int64_t value) {
  Type type = builder->getIndexType();
  ConstantOp::build(builder, result, type,
                    builder->getIntegerAttr(type, value));
}

//===----------------------------------------------------------------------===//
// DeallocOp
//===----------------------------------------------------------------------===//
namespace {
/// Fold Dealloc operations that are deallocating an AllocOp that is only used
/// by other Dealloc operations.
struct SimplifyDeadDealloc : public RewritePattern {
  SimplifyDeadDealloc(MLIRContext *context)
      : RewritePattern(DeallocOp::getOperationName(), 1, context) {}

  PatternMatchResult matchAndRewrite(Operation *op,
                                     PatternRewriter &rewriter) const override {
    auto dealloc = op->cast<DeallocOp>();

    // Check that the memref operand's defining operation is an AllocOp.
    Value *memref = dealloc.memref();
    Operation *defOp = memref->getDefiningOp();
    if (!isa_and_nonnull<AllocOp>(defOp))
      return matchFailure();

    // Check that all of the uses of the AllocOp are other DeallocOps.
    for (auto &use : memref->getUses())
      if (!use.getOwner()->isa<DeallocOp>())
        return matchFailure();

    // Erase the dealloc operation.
    op->erase();
    return matchSuccess();
  }
};
} // end anonymous namespace.

static void printDeallocOp(OpAsmPrinter *p, DeallocOp op) {
  *p << "dealloc " << *op.memref() << " : " << op.memref()->getType();
}

static ParseResult parseDeallocOp(OpAsmParser *parser, OperationState *result) {
  OpAsmParser::OperandType memrefInfo;
  MemRefType type;

  return failure(parser->parseOperand(memrefInfo) ||
                 parser->parseColonType(type) ||
                 parser->resolveOperand(memrefInfo, type, result->operands));
}

static LogicalResult verify(DeallocOp op) {
  if (!op.memref()->getType().isa<MemRefType>())
    return op.emitOpError("operand must be a memref");
  return success();
}

void DeallocOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                            MLIRContext *context) {
  /// dealloc(memrefcast) -> dealloc
  results.push_back(
      llvm::make_unique<MemRefCastFolder>(getOperationName(), context));
  results.push_back(llvm::make_unique<SimplifyDeadDealloc>(context));
}

//===----------------------------------------------------------------------===//
// DimOp
//===----------------------------------------------------------------------===//

void DimOp::build(Builder *builder, OperationState *result,
                  Value *memrefOrTensor, unsigned index) {
  result->addOperands(memrefOrTensor);
  auto type = builder->getIndexType();
  result->addAttribute("index", builder->getIntegerAttr(type, index));
  result->types.push_back(type);
}

void DimOp::print(OpAsmPrinter *p) {
  *p << "dim " << *getOperand() << ", " << getIndex();
  p->printOptionalAttrDict(getAttrs(), /*elidedAttrs=*/{"index"});
  *p << " : " << getOperand()->getType();
}

ParseResult DimOp::parse(OpAsmParser *parser, OperationState *result) {
  OpAsmParser::OperandType operandInfo;
  IntegerAttr indexAttr;
  Type type;
  Type indexType = parser->getBuilder().getIndexType();

  return failure(parser->parseOperand(operandInfo) || parser->parseComma() ||
                 parser->parseAttribute(indexAttr, indexType, "index",
                                        result->attributes) ||
                 parser->parseOptionalAttributeDict(result->attributes) ||
                 parser->parseColonType(type) ||
                 parser->resolveOperand(operandInfo, type, result->operands) ||
                 parser->addTypeToList(indexType, result->types));
}

LogicalResult DimOp::verify() {
  // Check that we have an integer index operand.
  auto indexAttr = getAttrOfType<IntegerAttr>("index");
  if (!indexAttr)
    return emitOpError("requires an integer attribute named 'index'");
  uint64_t index = indexAttr.getValue().getZExtValue();

  auto type = getOperand()->getType();
  if (auto tensorType = type.dyn_cast<RankedTensorType>()) {
    if (index >= static_cast<uint64_t>(tensorType.getRank()))
      return emitOpError("index is out of range");
  } else if (auto memrefType = type.dyn_cast<MemRefType>()) {
    if (index >= memrefType.getRank())
      return emitOpError("index is out of range");

  } else if (type.isa<UnrankedTensorType>()) {
    // ok, assumed to be in-range.
  } else {
    return emitOpError("requires an operand with tensor or memref type");
  }

  return success();
}

Attribute DimOp::constantFold(ArrayRef<Attribute> operands,
                              MLIRContext *context) {
  // Constant fold dim when the size along the index referred to is a constant.
  auto opType = getOperand()->getType();
  int64_t indexSize = -1;
  if (auto tensorType = opType.dyn_cast<RankedTensorType>()) {
    indexSize = tensorType.getShape()[getIndex()];
  } else if (auto memrefType = opType.dyn_cast<MemRefType>()) {
    indexSize = memrefType.getShape()[getIndex()];
  }

  if (indexSize >= 0)
    return IntegerAttr::get(IndexType::get(context), indexSize);

  return nullptr;
}

//===----------------------------------------------------------------------===//
// DivISOp
//===----------------------------------------------------------------------===//

Attribute DivISOp::constantFold(ArrayRef<Attribute> operands,
                                MLIRContext *context) {
  assert(operands.size() == 2 && "binary operation takes two operands");
  (void)context;

  auto lhs = operands.front().dyn_cast_or_null<IntegerAttr>();
  auto rhs = operands.back().dyn_cast_or_null<IntegerAttr>();
  if (!lhs || !rhs)
    return {};

  // Don't fold if it requires division by zero.
  if (rhs.getValue().isNullValue()) {
    return {};
  }

  // Don't fold if it would overflow.
  bool overflow;
  auto result = lhs.getValue().sdiv_ov(rhs.getValue(), overflow);
  return overflow ? IntegerAttr{} : IntegerAttr::get(lhs.getType(), result);
}

//===----------------------------------------------------------------------===//
// DivIUOp
//===----------------------------------------------------------------------===//

Attribute DivIUOp::constantFold(ArrayRef<Attribute> operands,
                                MLIRContext *context) {
  assert(operands.size() == 2 && "binary operation takes two operands");
  (void)context;

  auto lhs = operands.front().dyn_cast_or_null<IntegerAttr>();
  auto rhs = operands.back().dyn_cast_or_null<IntegerAttr>();
  if (!lhs || !rhs)
    return {};

  // Don't fold if it requires division by zero.
  if (rhs.getValue().isNullValue()) {
    return {};
  }

  return IntegerAttr::get(lhs.getType(), lhs.getValue().udiv(rhs.getValue()));
}

// ---------------------------------------------------------------------------
// DmaStartOp
// ---------------------------------------------------------------------------

void DmaStartOp::build(Builder *builder, OperationState *result,
                       Value *srcMemRef, ArrayRef<Value *> srcIndices,
                       Value *destMemRef, ArrayRef<Value *> destIndices,
                       Value *numElements, Value *tagMemRef,
                       ArrayRef<Value *> tagIndices, Value *stride,
                       Value *elementsPerStride) {
  result->addOperands(srcMemRef);
  result->addOperands(srcIndices);
  result->addOperands(destMemRef);
  result->addOperands(destIndices);
  result->addOperands(numElements);
  result->addOperands(tagMemRef);
  result->addOperands(tagIndices);
  if (stride) {
    result->addOperands(stride);
    result->addOperands(elementsPerStride);
  }
}

void DmaStartOp::print(OpAsmPrinter *p) {
  *p << "dma_start " << *getSrcMemRef() << '[';
  p->printOperands(getSrcIndices());
  *p << "], " << *getDstMemRef() << '[';
  p->printOperands(getDstIndices());
  *p << "], " << *getNumElements();
  *p << ", " << *getTagMemRef() << '[';
  p->printOperands(getTagIndices());
  *p << ']';
  if (isStrided()) {
    *p << ", " << *getStride();
    *p << ", " << *getNumElementsPerStride();
  }
  p->printOptionalAttrDict(getAttrs());
  *p << " : " << getSrcMemRef()->getType();
  *p << ", " << getDstMemRef()->getType();
  *p << ", " << getTagMemRef()->getType();
}

// Parse DmaStartOp.
// Ex:
//   %dma_id = dma_start %src[%i, %j], %dst[%k, %l], %size,
//                       %tag[%index], %stride, %num_elt_per_stride :
//                     : memref<3076 x f32, 0>,
//                       memref<1024 x f32, 2>,
//                       memref<1 x i32>
//
ParseResult DmaStartOp::parse(OpAsmParser *parser, OperationState *result) {
  OpAsmParser::OperandType srcMemRefInfo;
  SmallVector<OpAsmParser::OperandType, 4> srcIndexInfos;
  OpAsmParser::OperandType dstMemRefInfo;
  SmallVector<OpAsmParser::OperandType, 4> dstIndexInfos;
  OpAsmParser::OperandType numElementsInfo;
  OpAsmParser::OperandType tagMemrefInfo;
  SmallVector<OpAsmParser::OperandType, 4> tagIndexInfos;
  SmallVector<OpAsmParser::OperandType, 2> strideInfo;

  SmallVector<Type, 3> types;
  auto indexType = parser->getBuilder().getIndexType();

  // Parse and resolve the following list of operands:
  // *) source memref followed by its indices (in square brackets).
  // *) destination memref followed by its indices (in square brackets).
  // *) dma size in KiB.
  if (parser->parseOperand(srcMemRefInfo) ||
      parser->parseOperandList(srcIndexInfos, -1,
                               OpAsmParser::Delimiter::Square) ||
      parser->parseComma() || parser->parseOperand(dstMemRefInfo) ||
      parser->parseOperandList(dstIndexInfos, -1,
                               OpAsmParser::Delimiter::Square) ||
      parser->parseComma() || parser->parseOperand(numElementsInfo) ||
      parser->parseComma() || parser->parseOperand(tagMemrefInfo) ||
      parser->parseOperandList(tagIndexInfos, -1,
                               OpAsmParser::Delimiter::Square))
    return failure();

  // Parse optional stride and elements per stride.
  if (parser->parseTrailingOperandList(strideInfo)) {
    return failure();
  }
  if (!strideInfo.empty() && strideInfo.size() != 2) {
    return parser->emitError(parser->getNameLoc(),
                             "expected two stride related operands");
  }
  bool isStrided = strideInfo.size() == 2;

  if (parser->parseColonTypeList(types))
    return failure();

  if (types.size() != 3)
    return parser->emitError(parser->getNameLoc(), "fewer/more types expected");

  if (parser->resolveOperand(srcMemRefInfo, types[0], result->operands) ||
      parser->resolveOperands(srcIndexInfos, indexType, result->operands) ||
      parser->resolveOperand(dstMemRefInfo, types[1], result->operands) ||
      parser->resolveOperands(dstIndexInfos, indexType, result->operands) ||
      // size should be an index.
      parser->resolveOperand(numElementsInfo, indexType, result->operands) ||
      parser->resolveOperand(tagMemrefInfo, types[2], result->operands) ||
      // tag indices should be index.
      parser->resolveOperands(tagIndexInfos, indexType, result->operands))
    return failure();

  if (!types[0].isa<MemRefType>())
    return parser->emitError(parser->getNameLoc(),
                             "expected source to be of memref type");

  if (!types[1].isa<MemRefType>())
    return parser->emitError(parser->getNameLoc(),
                             "expected destination to be of memref type");

  if (!types[2].isa<MemRefType>())
    return parser->emitError(parser->getNameLoc(),
                             "expected tag to be of memref type");

  if (isStrided) {
    if (parser->resolveOperand(strideInfo[0], indexType, result->operands) ||
        parser->resolveOperand(strideInfo[1], indexType, result->operands))
      return failure();
  }

  // Check that source/destination index list size matches associated rank.
  if (srcIndexInfos.size() != types[0].cast<MemRefType>().getRank() ||
      dstIndexInfos.size() != types[1].cast<MemRefType>().getRank())
    return parser->emitError(parser->getNameLoc(),
                             "memref rank not equal to indices count");

  if (tagIndexInfos.size() != types[2].cast<MemRefType>().getRank())
    return parser->emitError(parser->getNameLoc(),
                             "tag memref rank not equal to indices count");

  return success();
}

LogicalResult DmaStartOp::verify() {
  // DMAs from different memory spaces supported.
  if (getSrcMemorySpace() == getDstMemorySpace()) {
    return emitOpError("DMA should be between different memory spaces");
  }

  if (getNumOperands() != getTagMemRefRank() + getSrcMemRefRank() +
                              getDstMemRefRank() + 3 + 1 &&
      getNumOperands() != getTagMemRefRank() + getSrcMemRefRank() +
                              getDstMemRefRank() + 3 + 1 + 2) {
    return emitOpError("incorrect number of operands");
  }
  return success();
}

void DmaStartOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                             MLIRContext *context) {
  /// dma_start(memrefcast) -> dma_start
  results.push_back(
      llvm::make_unique<MemRefCastFolder>(getOperationName(), context));
}

// ---------------------------------------------------------------------------
// DmaWaitOp
// ---------------------------------------------------------------------------

void DmaWaitOp::build(Builder *builder, OperationState *result,
                      Value *tagMemRef, ArrayRef<Value *> tagIndices,
                      Value *numElements) {
  result->addOperands(tagMemRef);
  result->addOperands(tagIndices);
  result->addOperands(numElements);
}

void DmaWaitOp::print(OpAsmPrinter *p) {
  *p << "dma_wait ";
  // Print operands.
  p->printOperand(getTagMemRef());
  *p << '[';
  p->printOperands(getTagIndices());
  *p << "], ";
  p->printOperand(getNumElements());
  p->printOptionalAttrDict(getAttrs());
  *p << " : " << getTagMemRef()->getType();
}

// Parse DmaWaitOp.
// Eg:
//   dma_wait %tag[%index], %num_elements : memref<1 x i32, (d0) -> (d0), 4>
//
ParseResult DmaWaitOp::parse(OpAsmParser *parser, OperationState *result) {
  OpAsmParser::OperandType tagMemrefInfo;
  SmallVector<OpAsmParser::OperandType, 2> tagIndexInfos;
  Type type;
  auto indexType = parser->getBuilder().getIndexType();
  OpAsmParser::OperandType numElementsInfo;

  // Parse tag memref, its indices, and dma size.
  if (parser->parseOperand(tagMemrefInfo) ||
      parser->parseOperandList(tagIndexInfos, -1,
                               OpAsmParser::Delimiter::Square) ||
      parser->parseComma() || parser->parseOperand(numElementsInfo) ||
      parser->parseColonType(type) ||
      parser->resolveOperand(tagMemrefInfo, type, result->operands) ||
      parser->resolveOperands(tagIndexInfos, indexType, result->operands) ||
      parser->resolveOperand(numElementsInfo, indexType, result->operands))
    return failure();

  if (!type.isa<MemRefType>())
    return parser->emitError(parser->getNameLoc(),
                             "expected tag to be of memref type");

  if (tagIndexInfos.size() != type.cast<MemRefType>().getRank())
    return parser->emitError(parser->getNameLoc(),
                             "tag memref rank not equal to indices count");

  return success();
}

void DmaWaitOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                            MLIRContext *context) {
  /// dma_wait(memrefcast) -> dma_wait
  results.push_back(
      llvm::make_unique<MemRefCastFolder>(getOperationName(), context));
}

//===----------------------------------------------------------------------===//
// ExtractElementOp
//===----------------------------------------------------------------------===//

void ExtractElementOp::build(Builder *builder, OperationState *result,
                             Value *aggregate, ArrayRef<Value *> indices) {
  auto aggregateType = aggregate->getType().cast<VectorOrTensorType>();
  result->addOperands(aggregate);
  result->addOperands(indices);
  result->types.push_back(aggregateType.getElementType());
}

void ExtractElementOp::print(OpAsmPrinter *p) {
  *p << "extract_element " << *getAggregate() << '[';
  p->printOperands(getIndices());
  *p << ']';
  p->printOptionalAttrDict(getAttrs());
  *p << " : " << getAggregate()->getType();
}

ParseResult ExtractElementOp::parse(OpAsmParser *parser,
                                    OperationState *result) {
  OpAsmParser::OperandType aggregateInfo;
  SmallVector<OpAsmParser::OperandType, 4> indexInfo;
  VectorOrTensorType type;

  auto affineIntTy = parser->getBuilder().getIndexType();
  return failure(
      parser->parseOperand(aggregateInfo) ||
      parser->parseOperandList(indexInfo, -1, OpAsmParser::Delimiter::Square) ||
      parser->parseOptionalAttributeDict(result->attributes) ||
      parser->parseColonType(type) ||
      parser->resolveOperand(aggregateInfo, type, result->operands) ||
      parser->resolveOperands(indexInfo, affineIntTy, result->operands) ||
      parser->addTypeToList(type.getElementType(), result->types));
}

LogicalResult ExtractElementOp::verify() {
  if (getNumOperands() == 0)
    return emitOpError("expected an aggregate to index into");

  auto aggregateType = getAggregate()->getType().dyn_cast<VectorOrTensorType>();
  if (!aggregateType)
    return emitOpError("first operand must be a vector or tensor");

  if (getType() != aggregateType.getElementType())
    return emitOpError("result type must match element type of aggregate");

  for (auto *idx : getIndices())
    if (!idx->getType().isIndex())
      return emitOpError("index to extract_element must have 'index' type");

  // Verify the # indices match if we have a ranked type.
  auto aggregateRank = aggregateType.getRank();
  if (aggregateRank != -1 && aggregateRank != getNumOperands() - 1)
    return emitOpError("incorrect number of indices for extract_element");

  return success();
}

Attribute ExtractElementOp::constantFold(ArrayRef<Attribute> operands,
                                         MLIRContext *context) {
  assert(!operands.empty() && "extract_element takes atleast one operand");

  // The aggregate operand must be a known constant.
  Attribute aggregate = operands.front();
  if (!aggregate)
    return Attribute();

  // If this is a splat elements attribute, simply return the value. All of the
  // elements of a splat attribute are the same.
  if (auto splatAggregate = aggregate.dyn_cast<SplatElementsAttr>())
    return splatAggregate.getValue();

  // Otherwise, collect the constant indices into the aggregate.
  SmallVector<uint64_t, 8> indices;
  for (Attribute indice : llvm::drop_begin(operands, 1)) {
    if (!indice || !indice.isa<IntegerAttr>())
      return Attribute();
    indices.push_back(indice.cast<IntegerAttr>().getInt());
  }

  // If this is an elements attribute, query the value at the given indices.
  if (auto elementsAttr = aggregate.dyn_cast<ElementsAttr>())
    return elementsAttr.getValue(indices);
  return Attribute();
}

//===----------------------------------------------------------------------===//
// LoadOp
//===----------------------------------------------------------------------===//

void LoadOp::build(Builder *builder, OperationState *result, Value *memref,
                   ArrayRef<Value *> indices) {
  auto memrefType = memref->getType().cast<MemRefType>();
  result->addOperands(memref);
  result->addOperands(indices);
  result->types.push_back(memrefType.getElementType());
}

void LoadOp::print(OpAsmPrinter *p) {
  *p << "load " << *getMemRef() << '[';
  p->printOperands(getIndices());
  *p << ']';
  p->printOptionalAttrDict(getAttrs());
  *p << " : " << getMemRefType();
}

ParseResult LoadOp::parse(OpAsmParser *parser, OperationState *result) {
  OpAsmParser::OperandType memrefInfo;
  SmallVector<OpAsmParser::OperandType, 4> indexInfo;
  MemRefType type;

  auto affineIntTy = parser->getBuilder().getIndexType();
  return failure(
      parser->parseOperand(memrefInfo) ||
      parser->parseOperandList(indexInfo, -1, OpAsmParser::Delimiter::Square) ||
      parser->parseOptionalAttributeDict(result->attributes) ||
      parser->parseColonType(type) ||
      parser->resolveOperand(memrefInfo, type, result->operands) ||
      parser->resolveOperands(indexInfo, affineIntTy, result->operands) ||
      parser->addTypeToList(type.getElementType(), result->types));
}

LogicalResult LoadOp::verify() {
  if (getNumOperands() == 0)
    return emitOpError("expected a memref to load from");

  auto memRefType = getMemRef()->getType().dyn_cast<MemRefType>();
  if (!memRefType)
    return emitOpError("first operand must be a memref");

  if (getType() != memRefType.getElementType())
    return emitOpError("result type must match element type of memref");

  if (memRefType.getRank() != getNumOperands() - 1)
    return emitOpError("incorrect number of indices for load");

  for (auto *idx : getIndices())
    if (!idx->getType().isIndex())
      return emitOpError("index to load must have 'index' type");

  // TODO: Verify we have the right number of indices.

  // TODO: in Function verify that the indices are parameters, IV's, or the
  // result of an affine.apply.
  return success();
}

void LoadOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                         MLIRContext *context) {
  /// load(memrefcast) -> load
  results.push_back(
      llvm::make_unique<MemRefCastFolder>(getOperationName(), context));
}

//===----------------------------------------------------------------------===//
// MemRefCastOp
//===----------------------------------------------------------------------===//

bool MemRefCastOp::areCastCompatible(Type a, Type b) {
  auto aT = a.dyn_cast<MemRefType>();
  auto bT = b.dyn_cast<MemRefType>();

  if (!aT || !bT)
    return false;
  if (aT.getElementType() != bT.getElementType())
    return false;
  if (aT.getAffineMaps() != bT.getAffineMaps())
    return false;
  if (aT.getMemorySpace() != bT.getMemorySpace())
    return false;

  // They must have the same rank, and any specified dimensions must match.
  if (aT.getRank() != bT.getRank())
    return false;

  for (unsigned i = 0, e = aT.getRank(); i != e; ++i) {
    int64_t aDim = aT.getDimSize(i), bDim = bT.getDimSize(i);
    if (aDim != -1 && bDim != -1 && aDim != bDim)
      return false;
  }

  return true;
}

void MemRefCastOp::print(OpAsmPrinter *p) {
  *p << "memref_cast " << *getOperand() << " : " << getOperand()->getType()
     << " to " << getType();
}

LogicalResult MemRefCastOp::verify() {
  auto opType = getOperand()->getType();
  auto resType = getType();
  if (!areCastCompatible(opType, resType))
    return emitError(llvm::formatv(
        "operand type {0} and result type {1} are cast incompatible", opType,
        resType));

  return success();
}

//===----------------------------------------------------------------------===//
// MulFOp
//===----------------------------------------------------------------------===//

Attribute MulFOp::constantFold(ArrayRef<Attribute> operands,
                               MLIRContext *context) {
  return constFoldBinaryOp<FloatAttr>(
      operands, [](APFloat a, APFloat b) { return a * b; });
}

//===----------------------------------------------------------------------===//
// MulIOp
//===----------------------------------------------------------------------===//

Attribute MulIOp::constantFold(ArrayRef<Attribute> operands,
                               MLIRContext *context) {
  // TODO: Handle the overflow case.
  return constFoldBinaryOp<IntegerAttr>(operands,
                                        [](APInt a, APInt b) { return a * b; });
}

Value *MulIOp::fold() {
  /// muli(x, 0) -> 0
  if (matchPattern(getOperand(1), m_Zero()))
    return getOperand(1);
  /// muli(x, 1) -> x
  if (matchPattern(getOperand(1), m_One()))
    return getOperand(0);
  return nullptr;
}

//===----------------------------------------------------------------------===//
// RemISOp
//===----------------------------------------------------------------------===//

Attribute RemISOp::constantFold(ArrayRef<Attribute> operands,
                                MLIRContext *context) {
  assert(operands.size() == 2 && "remis takes two operands");

  auto rhs = operands.back().dyn_cast_or_null<IntegerAttr>();
  if (!rhs)
    return {};

  // x % 1 = 0
  if (rhs.getValue().isOneValue())
    return IntegerAttr::get(rhs.getType(),
                            APInt(rhs.getValue().getBitWidth(), 0));

  // Don't fold if it requires division by zero.
  if (rhs.getValue().isNullValue()) {
    return {};
  }

  auto lhs = operands.front().dyn_cast_or_null<IntegerAttr>();
  if (!lhs)
    return {};

  return IntegerAttr::get(lhs.getType(), lhs.getValue().srem(rhs.getValue()));
}

//===----------------------------------------------------------------------===//
// RemIUOp
//===----------------------------------------------------------------------===//

Attribute RemIUOp::constantFold(ArrayRef<Attribute> operands,
                                MLIRContext *context) {
  assert(operands.size() == 2 && "remiu takes two operands");

  auto rhs = operands.back().dyn_cast_or_null<IntegerAttr>();
  if (!rhs)
    return {};

  // x % 1 = 0
  if (rhs.getValue().isOneValue())
    return IntegerAttr::get(rhs.getType(),
                            APInt(rhs.getValue().getBitWidth(), 0));

  // Don't fold if it requires division by zero.
  if (rhs.getValue().isNullValue()) {
    return {};
  }

  auto lhs = operands.front().dyn_cast_or_null<IntegerAttr>();
  if (!lhs)
    return {};

  return IntegerAttr::get(lhs.getType(), lhs.getValue().urem(rhs.getValue()));
}

//===----------------------------------------------------------------------===//
// ReturnOp
//===----------------------------------------------------------------------===//

void ReturnOp::build(Builder *builder, OperationState *result,
                     ArrayRef<Value *> results) {
  result->addOperands(results);
}

ParseResult ReturnOp::parse(OpAsmParser *parser, OperationState *result) {
  SmallVector<OpAsmParser::OperandType, 2> opInfo;
  SmallVector<Type, 2> types;
  llvm::SMLoc loc;
  return failure(parser->getCurrentLocation(&loc) ||
                 parser->parseOperandList(opInfo) ||
                 (!opInfo.empty() && parser->parseColonTypeList(types)) ||
                 parser->resolveOperands(opInfo, types, loc, result->operands));
}

void ReturnOp::print(OpAsmPrinter *p) {
  *p << "return";
  if (getNumOperands() > 0) {
    *p << ' ';
    p->printOperands(operand_begin(), operand_end());
    *p << " : ";
    interleave(
        operand_begin(), operand_end(),
        [&](Value *e) { p->printType(e->getType()); }, [&]() { *p << ", "; });
  }
}

LogicalResult ReturnOp::verify() {
  auto *function = getOperation()->getFunction();

  // The operand number and types must match the function signature.
  const auto &results = function->getType().getResults();
  if (getNumOperands() != results.size())
    return emitOpError("has " + Twine(getNumOperands()) +
                       " operands, but enclosing function returns " +
                       Twine(results.size()));

  for (unsigned i = 0, e = results.size(); i != e; ++i)
    if (getOperand(i)->getType() != results[i])
      return emitError("type of return operand " + Twine(i) +
                       " doesn't match function result type");

  return success();
}

//===----------------------------------------------------------------------===//
// SelectOp
//===----------------------------------------------------------------------===//

void SelectOp::build(Builder *builder, OperationState *result, Value *condition,
                     Value *trueValue, Value *falseValue) {
  result->addOperands({condition, trueValue, falseValue});
  result->addTypes(trueValue->getType());
}

ParseResult SelectOp::parse(OpAsmParser *parser, OperationState *result) {
  SmallVector<OpAsmParser::OperandType, 3> ops;
  SmallVector<NamedAttribute, 4> attrs;
  Type type;

  if (parser->parseOperandList(ops, 3) ||
      parser->parseOptionalAttributeDict(result->attributes) ||
      parser->parseColonType(type))
    return failure();

  auto i1Type = getCheckedI1SameShape(&parser->getBuilder(), type);
  if (!i1Type)
    return parser->emitError(parser->getNameLoc(),
                             "expected type with valid i1 shape");

  SmallVector<Type, 3> types = {i1Type, type, type};
  return failure(parser->resolveOperands(ops, types, parser->getNameLoc(),
                                         result->operands) ||
                 parser->addTypeToList(type, result->types));
}

void SelectOp::print(OpAsmPrinter *p) {
  *p << "select ";
  p->printOperands(getOperation()->getOperands());
  *p << " : " << getTrueValue()->getType();
  p->printOptionalAttrDict(getAttrs());
}

LogicalResult SelectOp::verify() {
  auto conditionType = getCondition()->getType();
  auto trueType = getTrueValue()->getType();
  auto falseType = getFalseValue()->getType();

  if (trueType != falseType)
    return emitOpError(
        "requires 'true' and 'false' arguments to be of the same type");

  if (checkI1SameShape(trueType, conditionType))
    return emitOpError("requires the condition to have the same shape as "
                       "arguments with elemental type i1");

  return success();
}

Value *SelectOp::fold() {
  auto *condition = getCondition();

  // select true, %0, %1 => %0
  if (matchPattern(condition, m_One()))
    return getTrueValue();

  // select false, %0, %1 => %1
  if (matchPattern(condition, m_Zero()))
    return getFalseValue();
  return nullptr;
}

//===----------------------------------------------------------------------===//
// StoreOp
//===----------------------------------------------------------------------===//

void StoreOp::build(Builder *builder, OperationState *result,
                    Value *valueToStore, Value *memref,
                    ArrayRef<Value *> indices) {
  result->addOperands(valueToStore);
  result->addOperands(memref);
  result->addOperands(indices);
}

void StoreOp::print(OpAsmPrinter *p) {
  *p << "store " << *getValueToStore();
  *p << ", " << *getMemRef() << '[';
  p->printOperands(getIndices());
  *p << ']';
  p->printOptionalAttrDict(getAttrs());
  *p << " : " << getMemRefType();
}

ParseResult StoreOp::parse(OpAsmParser *parser, OperationState *result) {
  OpAsmParser::OperandType storeValueInfo;
  OpAsmParser::OperandType memrefInfo;
  SmallVector<OpAsmParser::OperandType, 4> indexInfo;
  MemRefType memrefType;

  auto affineIntTy = parser->getBuilder().getIndexType();
  return failure(
      parser->parseOperand(storeValueInfo) || parser->parseComma() ||
      parser->parseOperand(memrefInfo) ||
      parser->parseOperandList(indexInfo, -1, OpAsmParser::Delimiter::Square) ||
      parser->parseOptionalAttributeDict(result->attributes) ||
      parser->parseColonType(memrefType) ||
      parser->resolveOperand(storeValueInfo, memrefType.getElementType(),
                             result->operands) ||
      parser->resolveOperand(memrefInfo, memrefType, result->operands) ||
      parser->resolveOperands(indexInfo, affineIntTy, result->operands));
}

LogicalResult StoreOp::verify() {
  if (getNumOperands() < 2)
    return emitOpError("expected a value to store and a memref");

  // Second operand is a memref type.
  auto memRefType = getMemRef()->getType().dyn_cast<MemRefType>();
  if (!memRefType)
    return emitOpError("second operand must be a memref");

  // First operand must have same type as memref element type.
  if (getValueToStore()->getType() != memRefType.getElementType())
    return emitOpError("first operand must have same type memref element type");

  if (getNumOperands() != 2 + memRefType.getRank())
    return emitOpError("store index operand count not equal to memref rank");

  for (auto *idx : getIndices())
    if (!idx->getType().isIndex())
      return emitOpError("index to load must have 'index' type");

  // TODO: Verify we have the right number of indices.

  // TODO: in Function verify that the indices are parameters, IV's, or the
  // result of an affine.apply.
  return success();
}

void StoreOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context) {
  /// store(memrefcast) -> store
  results.push_back(
      llvm::make_unique<MemRefCastFolder>(getOperationName(), context));
}

//===----------------------------------------------------------------------===//
// SubFOp
//===----------------------------------------------------------------------===//

Attribute SubFOp::constantFold(ArrayRef<Attribute> operands,
                               MLIRContext *context) {
  return constFoldBinaryOp<FloatAttr>(
      operands, [](APFloat a, APFloat b) { return a - b; });
}

//===----------------------------------------------------------------------===//
// SubIOp
//===----------------------------------------------------------------------===//

Attribute SubIOp::constantFold(ArrayRef<Attribute> operands,
                               MLIRContext *context) {
  return constFoldBinaryOp<IntegerAttr>(operands,
                                        [](APInt a, APInt b) { return a - b; });
}

namespace {
/// subi(x,x) -> 0
///
struct SimplifyXMinusX : public RewritePattern {
  SimplifyXMinusX(MLIRContext *context)
      : RewritePattern(SubIOp::getOperationName(), 1, context) {}

  PatternMatchResult matchAndRewrite(Operation *op,
                                     PatternRewriter &rewriter) const override {
    auto subi = op->cast<SubIOp>();
    if (subi.getOperand(0) != subi.getOperand(1))
      return matchFailure();

    rewriter.replaceOpWithNewOp<ConstantOp>(
        op, subi.getType(), rewriter.getZeroAttr(subi.getType()));
    return matchSuccess();
  }
};
} // end anonymous namespace.

void SubIOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                         MLIRContext *context) {
  results.push_back(llvm::make_unique<SimplifyXMinusX>(context));
}

//===----------------------------------------------------------------------===//
// AndOp
//===----------------------------------------------------------------------===//

Attribute AndOp::constantFold(ArrayRef<Attribute> operands,
                              MLIRContext *context) {
  return constFoldBinaryOp<IntegerAttr>(operands,
                                        [](APInt a, APInt b) { return a & b; });
}

Value *AndOp::fold() {
  /// and(x, 0) -> 0
  if (matchPattern(rhs(), m_Zero()))
    return rhs();
  /// and(x,x) -> x
  if (lhs() == rhs())
    return rhs();

  return nullptr;
}

//===----------------------------------------------------------------------===//
// OrOp
//===----------------------------------------------------------------------===//

Attribute OrOp::constantFold(ArrayRef<Attribute> operands,
                             MLIRContext *context) {
  return constFoldBinaryOp<IntegerAttr>(operands,
                                        [](APInt a, APInt b) { return a | b; });
}

Value *OrOp::fold() {
  /// or(x, 0) -> x
  if (matchPattern(rhs(), m_Zero()))
    return lhs();
  /// or(x,x) -> x
  if (lhs() == rhs())
    return rhs();

  return nullptr;
}

//===----------------------------------------------------------------------===//
// XOrOp
//===----------------------------------------------------------------------===//

Attribute XOrOp::constantFold(ArrayRef<Attribute> operands,
                              MLIRContext *context) {
  return constFoldBinaryOp<IntegerAttr>(operands,
                                        [](APInt a, APInt b) { return a ^ b; });
}

Value *XOrOp::fold() {
  /// xor(x, 0) -> x
  if (matchPattern(rhs(), m_Zero()))
    return lhs();

  return nullptr;
}

namespace {
/// xor(x,x) -> 0
///
struct SimplifyXXOrX : public RewritePattern {
  SimplifyXXOrX(MLIRContext *context)
      : RewritePattern(XOrOp::getOperationName(), 1, context) {}

  PatternMatchResult matchAndRewrite(Operation *op,
                                     PatternRewriter &rewriter) const override {
    auto xorOp = op->cast<XOrOp>();
    if (xorOp.lhs() != xorOp.rhs())
      return matchFailure();

    rewriter.replaceOpWithNewOp<ConstantOp>(
        op, xorOp.getType(), rewriter.getZeroAttr(xorOp.getType()));
    return matchSuccess();
  }
};
} // end anonymous namespace.

void XOrOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                        MLIRContext *context) {
  results.push_back(llvm::make_unique<SimplifyXXOrX>(context));
}
//===----------------------------------------------------------------------===//
// TensorCastOp
//===----------------------------------------------------------------------===//

bool TensorCastOp::areCastCompatible(Type a, Type b) {
  auto aT = a.dyn_cast<TensorType>();
  auto bT = b.dyn_cast<TensorType>();
  if (!aT || !bT)
    return false;

  if (aT.getElementType() != bT.getElementType())
    return false;

  // If the either are unranked, then the cast is valid.
  auto aRType = aT.dyn_cast<RankedTensorType>();
  auto bRType = bT.dyn_cast<RankedTensorType>();
  if (!aRType || !bRType)
    return true;

  // If they are both ranked, they have to have the same rank, and any specified
  // dimensions must match.
  if (aRType.getRank() != bRType.getRank())
    return false;

  for (unsigned i = 0, e = aRType.getRank(); i != e; ++i) {
    int64_t aDim = aRType.getDimSize(i), bDim = bRType.getDimSize(i);
    if (aDim != -1 && bDim != -1 && aDim != bDim)
      return false;
  }

  return true;
}

void TensorCastOp::print(OpAsmPrinter *p) {
  *p << "tensor_cast " << *getOperand() << " : " << getOperand()->getType()
     << " to " << getType();
}

LogicalResult TensorCastOp::verify() {
  auto opType = getOperand()->getType();
  auto resType = getType();
  if (!areCastCompatible(opType, resType))
    return emitError(llvm::formatv(
        "operand type {0} and result type {1} are cast incompatible", opType,
        resType));

  return success();
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "mlir/StandardOps/Ops.cpp.inc"
