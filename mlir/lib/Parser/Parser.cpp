//===- Parser.cpp - MLIR Parser Implementation ----------------------------===//
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
// This file implements the parser for the MLIR textual form.
//
//===----------------------------------------------------------------------===//

#include "mlir/Parser.h"
#include "Lexer.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/Support/STLExtras.h"
#include "mlir/Transforms/Utils.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include <algorithm>
using namespace mlir;
using llvm::MemoryBuffer;
using llvm::SMLoc;
using llvm::SourceMgr;

/// Simple enum to make code read better in cases that would otherwise return a
/// bool value.  Failure is "true" in a boolean context.
enum ParseResult { ParseSuccess, ParseFailure };

namespace {
class Parser;

/// This class refers to all of the state maintained globally by the parser,
/// such as the current lexer position etc.  The Parser base class provides
/// methods to access this.
class ParserState {
public:
  ParserState(const llvm::SourceMgr &sourceMgr, Module *module)
      : context(module->getContext()), module(module), lex(sourceMgr, context),
        curToken(lex.lexToken()) {}

  ~ParserState() {
    // Destroy the forward references upon error.
    for (auto forwardRef : functionForwardRefs)
      delete forwardRef.second;
    functionForwardRefs.clear();
  }

  // A map from affine map identifier to AffineMap.
  llvm::StringMap<AffineMap> affineMapDefinitions;

  // A map from integer set identifier to IntegerSet.
  llvm::StringMap<IntegerSet> integerSetDefinitions;

  // A map from type alias identifier to Type.
  llvm::StringMap<Type> typeAliasDefinitions;

  // This keeps track of all forward references to functions along with the
  // temporary function used to represent them.
  llvm::DenseMap<Identifier, Function *> functionForwardRefs;

private:
  ParserState(const ParserState &) = delete;
  void operator=(const ParserState &) = delete;

  friend class Parser;

  // The context we're parsing into.
  MLIRContext *const context;

  // This is the module we are parsing into.
  Module *const module;

  // The lexer for the source file we're parsing.
  Lexer lex;

  // This is the next token that hasn't been consumed yet.
  Token curToken;
};
} // end anonymous namespace

namespace {

/// This class implement support for parsing global entities like types and
/// shared entities like SSA names.  It is intended to be subclassed by
/// specialized subparsers that include state, e.g. when a local symbol table.
class Parser {
public:
  Builder builder;

  Parser(ParserState &state) : builder(state.context), state(state) {}

  // Helper methods to get stuff from the parser-global state.
  ParserState &getState() const { return state; }
  MLIRContext *getContext() const { return state.context; }
  Module *getModule() { return state.module; }
  const llvm::SourceMgr &getSourceMgr() { return state.lex.getSourceMgr(); }

  /// Return the current token the parser is inspecting.
  const Token &getToken() const { return state.curToken; }
  StringRef getTokenSpelling() const { return state.curToken.getSpelling(); }

  /// Encode the specified source location information into an attribute for
  /// attachment to the IR.
  Location getEncodedSourceLocation(llvm::SMLoc loc) {
    return state.lex.getEncodedSourceLocation(loc);
  }

  /// Emit an error and return failure.
  ParseResult emitError(const Twine &message) {
    return emitError(state.curToken.getLoc(), message);
  }
  ParseResult emitError(SMLoc loc, const Twine &message);

  /// Advance the current lexer onto the next token.
  void consumeToken() {
    assert(state.curToken.isNot(Token::eof, Token::error) &&
           "shouldn't advance past EOF or errors");
    state.curToken = state.lex.lexToken();
  }

  /// Advance the current lexer onto the next token, asserting what the expected
  /// current token is.  This is preferred to the above method because it leads
  /// to more self-documenting code with better checking.
  void consumeToken(Token::Kind kind) {
    assert(state.curToken.is(kind) && "consumed an unexpected token");
    consumeToken();
  }

  /// If the current token has the specified kind, consume it and return true.
  /// If not, return false.
  bool consumeIf(Token::Kind kind) {
    if (state.curToken.isNot(kind))
      return false;
    consumeToken(kind);
    return true;
  }

  /// Consume the specified token if present and return success.  On failure,
  /// output a diagnostic and return failure.
  ParseResult parseToken(Token::Kind expectedToken, const Twine &message);

  /// Parse a comma-separated list of elements up until the specified end token.
  ParseResult
  parseCommaSeparatedListUntil(Token::Kind rightToken,
                               const std::function<ParseResult()> &parseElement,
                               bool allowEmptyList = true);

  /// Parse a comma separated list of elements that must have at least one entry
  /// in it.
  ParseResult
  parseCommaSeparatedList(const std::function<ParseResult()> &parseElement);

  // We have two forms of parsing methods - those that return a non-null
  // pointer on success, and those that return a ParseResult to indicate whether
  // they returned a failure.  The second class fills in by-reference arguments
  // as the results of their action.

  // Type parsing.
  VectorType parseVectorType();
  ParseResult parseXInDimensionList();
  ParseResult parseDimensionListRanked(SmallVectorImpl<int64_t> &dimensions,
                                       bool allowDynamic);
  Type parseExtendedType();
  Type parseTensorType();
  Type parseTupleType();
  Type parseMemRefType();
  Type parseFunctionType();
  Type parseNonFunctionType();
  Type parseType();
  ParseResult parseTypeListNoParens(SmallVectorImpl<Type> &elements);
  ParseResult parseTypeListParens(SmallVectorImpl<Type> &elements);
  ParseResult parseFunctionResultTypes(SmallVectorImpl<Type> &elements);

  // Attribute parsing.
  Function *resolveFunctionReference(StringRef nameStr, SMLoc nameLoc,
                                     FunctionType type);
  Attribute parseAttribute(Type type = {});

  ParseResult parseAttributeDict(SmallVectorImpl<NamedAttribute> &attributes);

  // Polyhedral structures.
  AffineMap parseAffineMapReference();
  IntegerSet parseIntegerSetReference();
  ParseResult parseAffineMapOrIntegerSetReference(AffineMap &map,
                                                  IntegerSet &set);
  DenseElementsAttr parseDenseElementsAttr(VectorOrTensorType type);
  DenseElementsAttr parseDenseElementsAttrAsTensor(Type eltType);
  VectorOrTensorType parseVectorOrTensorType();

  // Location Parsing.

  /// Trailing locations.
  ///
  ///   trailing-location     ::= location?
  ///
  template <typename Owner>
  ParseResult parseOptionalTrailingLocation(Owner *owner) {
    // If there is a 'loc' we parse a trailing location.
    if (!getToken().is(Token::kw_loc))
      return ParseSuccess;

    // Parse the location.
    llvm::Optional<Location> directLoc;
    if (parseLocation(&directLoc))
      return ParseFailure;
    owner->setLoc(*directLoc);
    return ParseSuccess;
  }

  /// Parse an inline location.
  ParseResult parseLocation(llvm::Optional<Location> *loc);

  /// Parse a raw location instance.
  ParseResult parseLocationInstance(llvm::Optional<Location> *loc);

private:
  // The Parser is subclassed and reinstantiated.  Do not add additional
  // non-trivial state here, add it to the ParserState class.
  ParserState &state;
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Helper methods.
//===----------------------------------------------------------------------===//

ParseResult Parser::emitError(SMLoc loc, const Twine &message) {
  // If we hit a parse error in response to a lexer error, then the lexer
  // already reported the error.
  if (getToken().is(Token::error))
    return ParseFailure;

  getContext()->emitError(getEncodedSourceLocation(loc), message);
  return ParseFailure;
}

/// Consume the specified token if present and return success.  On failure,
/// output a diagnostic and return failure.
ParseResult Parser::parseToken(Token::Kind expectedToken,
                               const Twine &message) {
  if (consumeIf(expectedToken))
    return ParseSuccess;
  return emitError(message);
}

/// Parse a comma separated list of elements that must have at least one entry
/// in it.
ParseResult Parser::parseCommaSeparatedList(
    const std::function<ParseResult()> &parseElement) {
  // Non-empty case starts with an element.
  if (parseElement())
    return ParseFailure;

  // Otherwise we have a list of comma separated elements.
  while (consumeIf(Token::comma)) {
    if (parseElement())
      return ParseFailure;
  }
  return ParseSuccess;
}

/// Parse a comma-separated list of elements, terminated with an arbitrary
/// token.  This allows empty lists if allowEmptyList is true.
///
///   abstract-list ::= rightToken                  // if allowEmptyList == true
///   abstract-list ::= element (',' element)* rightToken
///
ParseResult Parser::parseCommaSeparatedListUntil(
    Token::Kind rightToken, const std::function<ParseResult()> &parseElement,
    bool allowEmptyList) {
  // Handle the empty case.
  if (getToken().is(rightToken)) {
    if (!allowEmptyList)
      return emitError("expected list element");
    consumeToken(rightToken);
    return ParseSuccess;
  }

  if (parseCommaSeparatedList(parseElement) ||
      parseToken(rightToken, "expected ',' or '" +
                                 Token::getTokenSpelling(rightToken) + "'"))
    return ParseFailure;

  return ParseSuccess;
}

//===----------------------------------------------------------------------===//
// Type Parsing
//===----------------------------------------------------------------------===//

/// Parse any type except the function type.
///
///   non-function-type ::= integer-type
///                       | index-type
///                       | float-type
///                       | extended-type
///                       | vector-type
///                       | tensor-type
///                       | memref-type
///                       | tuple-type
///
///   index-type ::= `index`
///   float-type ::= `f16` | `bf16` | `f32` | `f64`
///
Type Parser::parseNonFunctionType() {
  switch (getToken().getKind()) {
  default:
    return (emitError("expected non-function type"), nullptr);
  case Token::kw_memref:
    return parseMemRefType();
  case Token::kw_tensor:
    return parseTensorType();
  case Token::kw_tuple:
    return parseTupleType();
  case Token::kw_vector:
    return parseVectorType();
  // integer-type
  case Token::inttype: {
    auto width = getToken().getIntTypeBitwidth();
    if (!width.hasValue())
      return (emitError("invalid integer width"), nullptr);
    auto loc = getEncodedSourceLocation(getToken().getLoc());
    consumeToken(Token::inttype);
    return IntegerType::getChecked(width.getValue(), builder.getContext(), loc);
  }

  // float-type
  case Token::kw_bf16:
    consumeToken(Token::kw_bf16);
    return builder.getBF16Type();
  case Token::kw_f16:
    consumeToken(Token::kw_f16);
    return builder.getF16Type();
  case Token::kw_f32:
    consumeToken(Token::kw_f32);
    return builder.getF32Type();
  case Token::kw_f64:
    consumeToken(Token::kw_f64);
    return builder.getF64Type();

  // index-type
  case Token::kw_index:
    consumeToken(Token::kw_index);
    return builder.getIndexType();

  // extended type
  case Token::exclamation_identifier:
    return parseExtendedType();
  }
}

/// Parse an arbitrary type.
///
///   type ::= function-type
///          | non-function-type
///
Type Parser::parseType() {
  if (getToken().is(Token::l_paren))
    return parseFunctionType();
  return parseNonFunctionType();
}

/// Parse a vector type.
///
///   vector-type ::= `vector` `<` static-dimension-list primitive-type `>`
///   static-dimension-list ::= (decimal-literal `x`)+
///
VectorType Parser::parseVectorType() {
  consumeToken(Token::kw_vector);

  if (parseToken(Token::less, "expected '<' in vector type"))
    return nullptr;

  SmallVector<int64_t, 4> dimensions;
  if (parseDimensionListRanked(dimensions, /*allowDynamic=*/false))
    return nullptr;
  if (dimensions.empty())
    return (emitError("expected dimension size in vector type"), nullptr);

  // Parse the element type.
  auto typeLoc = getToken().getLoc();
  auto elementType = parseType();
  if (!elementType || parseToken(Token::greater, "expected '>' in vector type"))
    return nullptr;

  return VectorType::getChecked(dimensions, elementType,
                                getEncodedSourceLocation(typeLoc));
}

/// Parse an 'x' token in a dimension list, handling the case where the x is
/// juxtaposed with an element type, as in "xf32", leaving the "f32" as the next
/// token.
ParseResult Parser::parseXInDimensionList() {
  if (getToken().isNot(Token::bare_identifier) || getTokenSpelling()[0] != 'x')
    return emitError("expected 'x' in dimension list");

  // If we had a prefix of 'x', lex the next token immediately after the 'x'.
  if (getTokenSpelling().size() != 1)
    state.lex.resetPointer(getTokenSpelling().data() + 1);

  // Consume the 'x'.
  consumeToken(Token::bare_identifier);

  return ParseSuccess;
}

/// Parse a dimension list of a tensor or memref type.  This populates the
/// dimension list, using -1 for the `?` dimensions if `allowDynamic` is set and
/// errors out on `?` otherwise.
///
///   dimension-list-ranked ::= (dimension `x`)*
///   dimension ::= `?` | decimal-literal
///
/// When `allowDynamic` is not set, this can be also used to parse
///
///   static-dimension-list ::= (decimal-literal `x`)*
ParseResult
Parser::parseDimensionListRanked(SmallVectorImpl<int64_t> &dimensions,
                                 bool allowDynamic = true) {
  while (getToken().isAny(Token::integer, Token::question)) {
    if (consumeIf(Token::question)) {
      if (!allowDynamic)
        return emitError("expected static shape");
      dimensions.push_back(-1);
    } else {
      // Hexadecimal integer literals (starting with `0x`) are not allowed in
      // aggregate type declarations.  Therefore, `0xf32` should be processed as
      // a sequence of separate elements `0`, `x`, `f32`.
      if (getTokenSpelling().size() > 1 && getTokenSpelling()[1] == 'x') {
        // We can get here only if the token is an integer literal.  Hexadecimal
        // integer literals can only start with `0x` (`1x` wouldn't lex as a
        // literal, just `1` would, at which point we don't get into this
        // branch).
        assert(getTokenSpelling()[0] == '0' && "invalid integer literal");
        dimensions.push_back(0);
        state.lex.resetPointer(getTokenSpelling().data() + 1);
        consumeToken();
      } else {
        // Make sure this integer value is in bound and valid.
        auto dimension = getToken().getUnsignedIntegerValue();
        if (!dimension.hasValue())
          return emitError("invalid dimension");
        dimensions.push_back((int64_t)dimension.getValue());
        consumeToken(Token::integer);
      }
    }

    // Make sure we have an 'x' or something like 'xbf32'.
    if (parseXInDimensionList())
      return ParseFailure;
  }

  return ParseSuccess;
}

/// Parse an extended type.
///
///   extended-type ::= (dialect-type | type-alias)
///   dialect-type  ::= `!` dialect-namespace `<` '"' type-data '"' `>`
///   type-alias    ::= `!` alias-name
///
Type Parser::parseExtendedType() {
  assert(getToken().is(Token::exclamation_identifier));

  // Parse the dialect namespace.
  StringRef identifier = getTokenSpelling().drop_front();
  consumeToken(Token::exclamation_identifier);

  // If there is not a '<' token, we are parsing a type alias.
  if (getToken().isNot(Token::less)) {
    // Check for an alias for this type.
    auto aliasIt = state.typeAliasDefinitions.find(identifier);
    if (aliasIt == state.typeAliasDefinitions.end())
      return (emitError("undefined type alias id '" + identifier + "'"),
              nullptr);
    return aliasIt->second;
  }

  // Otherwise, we are parsing a dialect-specific type.

  // Consume the '<'.
  if (parseToken(Token::less, "expected '<' in dialect type"))
    return nullptr;

  // Parse the type specific data.
  if (getToken().isNot(Token::string))
    return (emitError("expected string literal type data in dialect type"),
            nullptr);

  auto typeData = getToken().getStringValue();
  auto loc = getEncodedSourceLocation(getToken().getLoc());
  consumeToken(Token::string);

  Type result;

  // If we found a registered dialect, then ask it to parse the type.
  if (auto *dialect = state.context->getRegisteredDialect(identifier)) {
    result = dialect->parseType(typeData, loc, state.context);
    if (!result)
      return nullptr;
  } else {
    // Otherwise, form a new unknown type.
    result = UnknownType::getChecked(Identifier::get(identifier, state.context),
                                     typeData, state.context, loc);
  }

  // Consume the '>'.
  if (parseToken(Token::greater, "expected '>' in dialect type"))
    return nullptr;
  return result;
}

/// Parse a tensor type.
///
///   tensor-type ::= `tensor` `<` dimension-list element-type `>`
///   dimension-list ::= dimension-list-ranked | `*x`
///
Type Parser::parseTensorType() {
  consumeToken(Token::kw_tensor);

  if (parseToken(Token::less, "expected '<' in tensor type"))
    return nullptr;

  bool isUnranked;
  SmallVector<int64_t, 4> dimensions;

  if (consumeIf(Token::star)) {
    // This is an unranked tensor type.
    isUnranked = true;

    if (parseXInDimensionList())
      return nullptr;

  } else {
    isUnranked = false;
    if (parseDimensionListRanked(dimensions))
      return nullptr;
  }

  // Parse the element type.
  auto typeLocation = getEncodedSourceLocation(getToken().getLoc());
  auto elementType = parseType();
  if (!elementType || parseToken(Token::greater, "expected '>' in tensor type"))
    return nullptr;

  if (isUnranked)
    return UnrankedTensorType::getChecked(elementType, typeLocation);
  return RankedTensorType::getChecked(dimensions, elementType, typeLocation);
}

/// Parse a tuple type.
///
///   tuple-type ::= `tuple` `<` (type (`,` type)*)? `>`
///
Type Parser::parseTupleType() {
  consumeToken(Token::kw_tuple);

  // Parse the '<'.
  if (parseToken(Token::less, "expected '<' in tuple type"))
    return nullptr;

  // Check for an empty tuple by directly parsing '>'.
  if (consumeIf(Token::greater))
    return TupleType::get(getContext());

  // Parse the element types and the '>'.
  SmallVector<Type, 4> types;
  if (parseTypeListNoParens(types) ||
      parseToken(Token::greater, "expected '>' in tuple type"))
    return nullptr;

  return TupleType::get(types, getContext());
}

/// Parse a memref type.
///
///   memref-type ::= `memref` `<` dimension-list-ranked element-type
///                   (`,` semi-affine-map-composition)? (`,` memory-space)? `>`
///
///   semi-affine-map-composition ::= (semi-affine-map `,` )* semi-affine-map
///   memory-space ::= integer-literal /* | TODO: address-space-id */
///
Type Parser::parseMemRefType() {
  consumeToken(Token::kw_memref);

  if (parseToken(Token::less, "expected '<' in memref type"))
    return nullptr;

  SmallVector<int64_t, 4> dimensions;
  if (parseDimensionListRanked(dimensions))
    return nullptr;

  // Parse the element type.
  auto typeLoc = getToken().getLoc();
  auto elementType = parseType();
  if (!elementType)
    return nullptr;

  // Parse semi-affine-map-composition.
  SmallVector<AffineMap, 2> affineMapComposition;
  unsigned memorySpace = 0;
  bool parsedMemorySpace = false;

  auto parseElt = [&]() -> ParseResult {
    if (getToken().is(Token::integer)) {
      // Parse memory space.
      if (parsedMemorySpace)
        return emitError("multiple memory spaces specified in memref type");
      auto v = getToken().getUnsignedIntegerValue();
      if (!v.hasValue())
        return emitError("invalid memory space in memref type");
      memorySpace = v.getValue();
      consumeToken(Token::integer);
      parsedMemorySpace = true;
    } else {
      // Parse affine map.
      if (parsedMemorySpace)
        return emitError("affine map after memory space in memref type");
      auto affineMap = parseAffineMapReference();
      if (!affineMap)
        return ParseFailure;
      affineMapComposition.push_back(affineMap);
    }
    return ParseSuccess;
  };

  // Parse a list of mappings and address space if present.
  if (consumeIf(Token::comma)) {
    // Parse comma separated list of affine maps, followed by memory space.
    if (parseCommaSeparatedListUntil(Token::greater, parseElt,
                                     /*allowEmptyList=*/false)) {
      return nullptr;
    }
  } else {
    if (parseToken(Token::greater, "expected ',' or '>' in memref type"))
      return nullptr;
  }

  return MemRefType::getChecked(dimensions, elementType, affineMapComposition,
                                memorySpace, getEncodedSourceLocation(typeLoc));
}

/// Parse a function type.
///
///   function-type ::= type-list-parens `->` type-list
///
Type Parser::parseFunctionType() {
  assert(getToken().is(Token::l_paren));

  SmallVector<Type, 4> arguments, results;
  if (parseTypeListParens(arguments) ||
      parseToken(Token::arrow, "expected '->' in function type") ||
      parseFunctionResultTypes(results))
    return nullptr;

  return builder.getFunctionType(arguments, results);
}

/// Parse a list of types without an enclosing parenthesis.  The list must have
/// at least one member.
///
///   type-list-no-parens ::=  type (`,` type)*
///
ParseResult Parser::parseTypeListNoParens(SmallVectorImpl<Type> &elements) {
  auto parseElt = [&]() -> ParseResult {
    auto elt = parseType();
    elements.push_back(elt);
    return elt ? ParseSuccess : ParseFailure;
  };

  return parseCommaSeparatedList(parseElt);
}

/// Parse a parenthesized list of types.
///
///   type-list-parens ::= `(` `)`
///                      | `(` type-list-no-parens `)`
///
ParseResult Parser::parseTypeListParens(SmallVectorImpl<Type> &elements) {
  if (parseToken(Token::l_paren, "expected '('"))
    return ParseFailure;

  // Handle empty lists.
  if (getToken().is(Token::r_paren))
    return consumeToken(), ParseSuccess;

  if (parseTypeListNoParens(elements) ||
      parseToken(Token::r_paren, "expected ')'"))
    return ParseFailure;
  return ParseSuccess;
}

/// Parse a function result type.
///
///   function-result-type ::= type-list-parens
///                          | non-function-type
///
ParseResult Parser::parseFunctionResultTypes(SmallVectorImpl<Type> &elements) {
  if (getToken().is(Token::l_paren))
    return parseTypeListParens(elements);

  Type t = parseNonFunctionType();
  if (!t)
    return ParseFailure;
  elements.push_back(t);
  return ParseSuccess;
}

//===----------------------------------------------------------------------===//
// Attribute parsing.
//===----------------------------------------------------------------------===//

namespace {
class TensorLiteralParser {
public:
  TensorLiteralParser(Parser &p, Type eltTy) : p(p), eltTy(eltTy) {}

  ParseResult parse() {
    if (p.getToken().isNot(Token::l_square))
      return p.emitError("expected '[' in tensor literal list");
    return parseList(shape);
  }

  ArrayRef<Attribute> getValues() const { return storage; }

  ArrayRef<int64_t> getShape() const { return shape; }

private:
  /// Parse either a single element or a list of elements. Return the dimensions
  /// of the parsed sub-tensor in dims.
  ParseResult parseElementOrList(llvm::SmallVectorImpl<int64_t> &dims);

  /// Parse a list of either lists or elements, returning the dimensions of the
  /// parsed sub-tensors in dims. For example:
  ///   parseList([1, 2, 3]) -> Success, [3]
  ///   parseList([[1, 2], [3, 4]]) -> Success, [2, 2]
  ///   parseList([[1, 2], 3]) -> Failure
  ///   parseList([[1, [2, 3]], [4, [5]]]) -> Failure
  ParseResult parseList(llvm::SmallVectorImpl<int64_t> &dims);

  Parser &p;
  Type eltTy;
  SmallVector<int64_t, 4> shape;
  std::vector<Attribute> storage;
};
} // namespace

/// Parse either a single element or a list of elements. Return the dimensions
/// of the parsed sub-tensor in dims.
ParseResult
TensorLiteralParser::parseElementOrList(llvm::SmallVectorImpl<int64_t> &dims) {
  switch (p.getToken().getKind()) {
  case Token::l_square:
    return parseList(dims);
  case Token::floatliteral:
  case Token::integer:
  case Token::minus: {
    auto result = p.parseAttribute(eltTy);
    if (!result)
      return ParseResult::ParseFailure;
    // check result matches the element type.
    switch (eltTy.getKind()) {
    case StandardTypes::BF16:
    case StandardTypes::F16:
    case StandardTypes::F32:
    case StandardTypes::F64: {
      // Bitcast the APFloat value to APInt and store the bit representation.
      auto fpAttrResult = result.dyn_cast<FloatAttr>();
      if (!fpAttrResult)
        return p.emitError(
            "expected tensor literal element with floating point type");
      auto apInt = fpAttrResult.getValue().bitcastToAPInt();

      // FIXME: using 64 bits and double semantics for BF16 because APFloat does
      // not support BF16 directly.
      size_t bitWidth = eltTy.isBF16() ? 64 : eltTy.getIntOrFloatBitWidth();
      assert(apInt.getBitWidth() == bitWidth);
      (void)bitWidth;
      (void)apInt;
      break;
    }
    case StandardTypes::Integer: {
      if (!result.isa<IntegerAttr>())
        return p.emitError("expected tensor literal element has integer type");
      auto value = result.cast<IntegerAttr>().getValue();
      if (value.getMinSignedBits() > eltTy.getIntOrFloatBitWidth())
        return p.emitError("tensor literal element has more bits than that "
                           "specified in the type");
      break;
    }
    default:
      return p.emitError("expected integer or float tensor element");
    }
    storage.push_back(result);
    break;
  }
  default:
    return p.emitError("expected '[' or scalar constant inside tensor literal");
  }
  return ParseSuccess;
}

/// Parse a list of either lists or elements, returning the dimensions of the
/// parsed sub-tensors in dims. For example:
///   parseList([1, 2, 3]) -> Success, [3]
///   parseList([[1, 2], [3, 4]]) -> Success, [2, 2]
///   parseList([[1, 2], 3]) -> Failure
///   parseList([[1, [2, 3]], [4, [5]]]) -> Failure
ParseResult
TensorLiteralParser::parseList(llvm::SmallVectorImpl<int64_t> &dims) {
  p.consumeToken(Token::l_square);

  auto checkDims = [&](const llvm::SmallVectorImpl<int64_t> &prevDims,
                       const llvm::SmallVectorImpl<int64_t> &newDims) {
    if (prevDims == newDims)
      return ParseSuccess;
    return p.emitError("tensor literal is invalid; ranks are not consistent "
                       "between elements");
  };

  bool first = true;
  llvm::SmallVector<int64_t, 4> newDims;
  unsigned size = 0;
  auto parseCommaSeparatedList = [&]() {
    llvm::SmallVector<int64_t, 4> thisDims;
    if (parseElementOrList(thisDims))
      return ParseFailure;
    ++size;
    if (!first)
      return checkDims(newDims, thisDims);
    newDims = thisDims;
    first = false;
    return ParseSuccess;
  };
  if (p.parseCommaSeparatedListUntil(Token::r_square, parseCommaSeparatedList))
    return ParseFailure;

  // Return the sublists' dimensions with 'size' prepended.
  dims.clear();
  dims.push_back(size);
  dims.append(newDims.begin(), newDims.end());
  return ParseSuccess;
}

/// Given a parsed reference to a function name like @foo and a type that it
/// corresponds to, resolve it to a concrete function object (possibly
/// synthesizing a forward reference) or emit an error and return null on
/// failure.
Function *Parser::resolveFunctionReference(StringRef nameStr, SMLoc nameLoc,
                                           FunctionType type) {
  Identifier name = builder.getIdentifier(nameStr.drop_front());

  // See if the function has already been defined in the module.
  Function *function = getModule()->getNamedFunction(name);

  // If not, get or create a forward reference to one.
  if (!function) {
    auto &entry = state.functionForwardRefs[name];
    if (!entry)
      entry = new Function(getEncodedSourceLocation(nameLoc), name, type,
                           /*attrs=*/{});
    function = entry;
  }

  if (function->getType() != type)
    return (emitError(nameLoc, "reference to function with mismatched type"),
            nullptr);
  return function;
}

/// Attribute parsing.
///
///  attribute-value ::= bool-literal
///                    | integer-literal (`:` (index-type | integer-type))?
///                    | float-literal (`:` float-type)?
///                    | string-literal
///                    | type
///                    | `[` (attribute-value (`,` attribute-value)*)? `]`
///                    | function-id `:` function-type
///                    | (`splat` | `dense`) `<` (tensor-type | vector-type) `,`
///                      attribute-value `>`
///                    | `sparse` `<` (tensor-type | vector-type)`,`
///                          attribute-value `,` attribute-value `>`
///                    | `opaque` `<` dialect-namespace  `,`
///                      (tensor-type | vector-type) `,` hex-string-literal `>`
///
Attribute Parser::parseAttribute(Type type) {
  switch (getToken().getKind()) {
  case Token::kw_true:
    consumeToken(Token::kw_true);
    return builder.getBoolAttr(true);
  case Token::kw_false:
    consumeToken(Token::kw_false);
    return builder.getBoolAttr(false);

  case Token::floatliteral: {
    auto val = getToken().getFloatingPointValue();
    if (!val.hasValue())
      return (emitError("floating point value too large for attribute"),
              nullptr);
    auto valTok = getToken().getLoc();
    consumeToken(Token::floatliteral);
    if (!type) {
      if (consumeIf(Token::colon)) {
        if (!(type = parseType()))
          return nullptr;
      } else {
        // Default to F64 when no type is specified.
        type = builder.getF64Type();
      }
    }
    if (!type.isa<FloatType>())
      return (emitError("floating point value not valid for specified type"),
              nullptr);
    return FloatAttr::getChecked(type, val.getValue(),
                                 getEncodedSourceLocation(valTok));
  }
  case Token::integer: {
    auto val = getToken().getUInt64IntegerValue();
    if (!val.hasValue() || (int64_t)val.getValue() < 0)
      return (emitError("integer constant out of range for attribute"),
              nullptr);
    consumeToken(Token::integer);
    if (!type) {
      if (consumeIf(Token::colon)) {
        if (!(type = parseType()))
          return nullptr;
      } else {
        // Default to i64 if not type is specified.
        type = builder.getIntegerType(64);
      }
    }
    if (!type.isIntOrIndex())
      return (emitError("integer value not valid for specified type"), nullptr);
    int width = type.isIndex() ? 64 : type.getIntOrFloatBitWidth();
    APInt apInt(width, val.getValue());
    if (apInt != *val)
      return emitError("integer constant out of range for attribute"), nullptr;
    return builder.getIntegerAttr(type, apInt);
  }

  case Token::minus: {
    consumeToken(Token::minus);
    if (getToken().is(Token::integer)) {
      auto val = getToken().getUInt64IntegerValue();
      if (!val.hasValue() || (int64_t)-val.getValue() >= 0)
        return (emitError("integer constant out of range for attribute"),
                nullptr);
      consumeToken(Token::integer);
      if (!type) {
        if (consumeIf(Token::colon)) {
          if (!(type = parseType()))
            return nullptr;
        } else {
          // Default to i64 if not type is specified.
          type = builder.getIntegerType(64);
        }
      }
      if (!type.isIntOrIndex())
        return (emitError("integer value not valid for type"), nullptr);
      int width = type.isIndex() ? 64 : type.getIntOrFloatBitWidth();
      APInt apInt(width, *val, /*isSigned=*/true);
      if (apInt != *val)
        return (emitError("integer constant out of range for attribute"),
                nullptr);
      return builder.getIntegerAttr(type, -apInt);
    }
    if (getToken().is(Token::floatliteral)) {
      auto val = getToken().getFloatingPointValue();
      if (!val.hasValue())
        return (emitError("floating point value too large for attribute"),
                nullptr);
      auto valTok = getToken().getLoc();
      consumeToken(Token::floatliteral);
      if (!type) {
        if (consumeIf(Token::colon)) {
          if (!(type = parseType()))
            return nullptr;
        } else {
          // Default to F64 when no type is specified.
          type = builder.getF64Type();
        }
      }
      if (!type.isa<FloatType>())
        return (emitError("floating point value not valid for type"), nullptr);
      return FloatAttr::getChecked(type, -val.getValue(),
                                   getEncodedSourceLocation(valTok));
    }

    return (emitError("expected constant integer or floating point value"),
            nullptr);
  }

  case Token::string: {
    auto val = getToken().getStringValue();
    consumeToken(Token::string);
    return builder.getStringAttr(val);
  }

  case Token::l_square: {
    consumeToken(Token::l_square);
    SmallVector<Attribute, 4> elements;

    auto parseElt = [&]() -> ParseResult {
      elements.push_back(parseAttribute());
      return elements.back() ? ParseSuccess : ParseFailure;
    };

    if (parseCommaSeparatedListUntil(Token::r_square, parseElt))
      return nullptr;
    return builder.getArrayAttr(elements);
  }
  case Token::hash_identifier:
  case Token::l_paren: {
    // Try to parse an affine map or an integer set reference.
    AffineMap map;
    IntegerSet set;
    if (parseAffineMapOrIntegerSetReference(map, set))
      return nullptr;
    if (map)
      return builder.getAffineMapAttr(map);
    assert(set);
    return builder.getIntegerSetAttr(set);
  }

  case Token::at_identifier: {
    auto nameLoc = getToken().getLoc();
    auto nameStr = getTokenSpelling();
    consumeToken(Token::at_identifier);

    if (parseToken(Token::colon, "expected ':' and function type"))
      return nullptr;
    auto typeLoc = getToken().getLoc();
    Type type = parseType();
    if (!type)
      return nullptr;
    auto fnType = type.dyn_cast<FunctionType>();
    if (!fnType)
      return (emitError(typeLoc, "expected function type"), nullptr);

    auto *function = resolveFunctionReference(nameStr, nameLoc, fnType);
    return function ? builder.getFunctionAttr(function) : nullptr;
  }
  case Token::kw_opaque: {
    consumeToken(Token::kw_opaque);
    if (parseToken(Token::less, "expected '<' after 'opaque'"))
      return nullptr;

    if (getToken().getKind() != Token::string)
      return (emitError("expected dialect namespace"), nullptr);
    auto name = getToken().getStringValue();
    auto *dialect = builder.getContext()->getRegisteredDialect(name);
    // TODO(shpeisman): Allow for having an unknown dialect on an opaque
    // attribute. Otherwise, it can't be roundtripped without having the dialect
    // registered.
    if (!dialect)
      return (emitError("no registered dialect with namespace '" + name + "'"),
              nullptr);

    consumeToken(Token::string);
    if (parseToken(Token::comma, "expected ','"))
      return nullptr;

    auto type = parseVectorOrTensorType();
    if (!type)
      return nullptr;

    if (getToken().getKind() != Token::string)
      return (emitError("opaque string should start with '0x'"), nullptr);
    auto val = getToken().getStringValue();
    if (val.size() < 2 || val[0] != '0' || val[1] != 'x')
      return (emitError("opaque string should start with '0x'"), nullptr);
    val = val.substr(2);
    if (!std::all_of(val.begin(), val.end(),
                     [](char c) { return llvm::isHexDigit(c); })) {
      return (emitError("opaque string only contains hex digits"), nullptr);
    }
    consumeToken(Token::string);
    if (parseToken(Token::greater, "expected '>'"))
      return nullptr;
    return builder.getOpaqueElementsAttr(dialect, type, llvm::fromHex(val));
  }
  case Token::kw_splat: {
    consumeToken(Token::kw_splat);
    if (parseToken(Token::less, "expected '<' after 'splat'"))
      return nullptr;

    auto type = parseVectorOrTensorType();
    if (!type)
      return nullptr;
    switch (getToken().getKind()) {
    case Token::floatliteral:
    case Token::integer:
    case Token::kw_false:
    case Token::kw_true:
    case Token::minus: {
      auto scalar = parseAttribute(type.getElementType());
      if (!scalar)
        return nullptr;
      if (parseToken(Token::greater, "expected '>'"))
        return nullptr;
      return builder.getSplatElementsAttr(type, scalar);
    }
    default:
      return (emitError("expected scalar constant inside tensor literal"),
              nullptr);
    }
  }
  case Token::kw_dense: {
    consumeToken(Token::kw_dense);
    if (parseToken(Token::less, "expected '<' after 'dense'"))
      return nullptr;

    auto type = parseVectorOrTensorType();
    if (!type)
      return nullptr;

    switch (getToken().getKind()) {
    case Token::l_square: {
      auto attr = parseDenseElementsAttr(type);
      if (!attr)
        return nullptr;
      if (parseToken(Token::greater, "expected '>'"))
        return nullptr;
      return attr;
    }
    default:
      return (emitError("expected '[' to start dense tensor literal"), nullptr);
    }
  }
  case Token::kw_sparse: {
    consumeToken(Token::kw_sparse);
    if (parseToken(Token::less, "Expected '<' after 'sparse'"))
      return nullptr;

    auto type = parseVectorOrTensorType();
    if (!type)
      return nullptr;

    switch (getToken().getKind()) {
    case Token::l_square: {
      /// Parse indices
      auto indicesEltType = builder.getIntegerType(64);
      auto indices = parseDenseElementsAttrAsTensor(indicesEltType);
      if (!indices)
        return nullptr;

      if (parseToken(Token::comma, "expected ','"))
        return nullptr;

      /// Parse values.
      auto valuesEltType = type.getElementType();
      auto values = parseDenseElementsAttrAsTensor(valuesEltType);
      if (!values)
        return nullptr;

      /// Sanity check.
      auto indicesType = indices.getType();
      auto valuesType = values.getType();
      auto sameShape = (indicesType.getRank() == 1) ||
                       (type.getRank() == indicesType.getDimSize(1));
      auto sameElementNum =
          indicesType.getDimSize(0) == valuesType.getDimSize(0);
      if (!sameShape || !sameElementNum) {
        std::string str;
        llvm::raw_string_ostream s(str);
        s << "expected shape ([";
        interleaveComma(type.getShape(), s);
        s << "]); inferred shape of indices literal ([";
        interleaveComma(indicesType.getShape(), s);
        s << "]); inferred shape of values literal ([";
        interleaveComma(valuesType.getShape(), s);
        s << "])";
        return (emitError(s.str()), nullptr);
      }

      if (parseToken(Token::greater, "expected '>'"))
        return nullptr;

      // Build the sparse elements attribute by the indices and values.
      return builder.getSparseElementsAttr(
          type, indices.cast<DenseIntElementsAttr>(), values);
    }
    default:
      return (emitError("expected '[' to start sparse tensor literal"),
              nullptr);
    }
    return (emitError("expected elements literal has a tensor or vector type"),
            nullptr);
  }
  default: {
    if (Type type = parseType())
      return builder.getTypeAttr(type);
    return nullptr;
  }
  }
}

/// Dense elements attribute.
///
///   dense-attr-list ::= `[` attribute-value `]`
///   attribute-value ::= integer-literal
///                     | float-literal
///                     | `[` (attribute-value (`,` attribute-value)*)? `]`
///
/// This method returns a constructed dense elements attribute of tensor type
/// with the shape from the parsing result.
DenseElementsAttr Parser::parseDenseElementsAttrAsTensor(Type eltType) {
  TensorLiteralParser literalParser(*this, eltType);
  if (literalParser.parse())
    return nullptr;

  auto type = builder.getTensorType(literalParser.getShape(), eltType);
  return builder.getDenseElementsAttr(type, literalParser.getValues())
      .cast<DenseElementsAttr>();
}

/// Dense elements attribute.
///
///   dense-attr-list ::= `[` attribute-value `]`
///   attribute-value ::= integer-literal
///                     | float-literal
///                     | `[` (attribute-value (`,` attribute-value)*)? `]`
///
/// This method compares the shapes from the parsing result and that from the
/// input argument. It returns a constructed dense elements attribute if both
/// match.
DenseElementsAttr Parser::parseDenseElementsAttr(VectorOrTensorType type) {
  auto eltTy = type.getElementType();
  TensorLiteralParser literalParser(*this, eltTy);
  if (literalParser.parse())
    return nullptr;
  if (literalParser.getShape() != type.getShape()) {
    std::string str;
    llvm::raw_string_ostream s(str);
    s << "inferred shape of elements literal ([";
    interleaveComma(literalParser.getShape(), s);
    s << "]) does not match type ([";
    interleaveComma(type.getShape(), s);
    s << "])";
    return (emitError(s.str()), nullptr);
  }
  return builder.getDenseElementsAttr(type, literalParser.getValues())
      .cast<DenseElementsAttr>();
}

/// Vector or tensor type for elements attribute.
///
///   vector-or-tensor-type ::= vector-type | tensor-type
///
/// This method also checks the type has static shape and ranked.
VectorOrTensorType Parser::parseVectorOrTensorType() {
  auto elementType = parseType();
  if (!elementType)
    return nullptr;

  auto type = elementType.dyn_cast<VectorOrTensorType>();
  if (!type) {
    return (emitError("expected elements literal has a tensor or vector type"),
            nullptr);
  }

  if (parseToken(Token::comma, "expected ','"))
    return nullptr;

  if (!type.hasStaticShape() || type.getRank() == -1) {
    return (emitError("tensor literals must be ranked and have static shape"),
            nullptr);
  }
  return type;
}

/// Debug Location.
///
///   location           ::= `loc` inline-location
///   inline-location    ::= '(' location-inst ')'
///
ParseResult Parser::parseLocation(llvm::Optional<Location> *loc) {
  assert(loc && "loc is expected to be non-null");

  // Check for 'loc' identifier.
  if (getToken().isNot(Token::kw_loc))
    return emitError("expected location keyword");
  consumeToken(Token::kw_loc);

  // Parse the inline-location.
  if (parseToken(Token::l_paren, "expected '(' in inline location") ||
      parseLocationInstance(loc) ||
      parseToken(Token::r_paren, "expected ')' in inline location"))
    return ParseFailure;
  return ParseSuccess;
}

/// Specific location instances.
///
/// location-inst ::= filelinecol-location |
///                   name-location |
///                   callsite-location |
///                   fused-location |
///                   unknown-location
/// filelinecol-location ::= string-literal ':' integer-literal
///                                         ':' integer-literal
/// name-location ::= string-literal
/// callsite-location ::= 'callsite' '(' location-inst 'at' location-inst ')'
/// fused-location ::= fused ('<' attribute-value '>')?
///                    '[' location-inst (location-inst ',')* ']'
/// unknown-location ::= 'unknown'
///
ParseResult Parser::parseLocationInstance(llvm::Optional<Location> *loc) {
  auto *ctx = getContext();

  // Handle either name or filelinecol locations.
  if (getToken().is(Token::string)) {
    auto str = getToken().getStringValue();
    consumeToken(Token::string);

    // If the next token is ':' this is a filelinecol location.
    if (consumeIf(Token::colon)) {
      // Parse the line number.
      if (getToken().isNot(Token::integer))
        return emitError("expected integer line number in FileLineColLoc");
      auto line = getToken().getUnsignedIntegerValue();
      if (!line.hasValue())
        return emitError("expected integer line number in FileLineColLoc");
      consumeToken(Token::integer);

      // Parse the ':'.
      if (parseToken(Token::colon, "expected ':' in FileLineColLoc"))
        return ParseFailure;

      // Parse the column number.
      if (getToken().isNot(Token::integer))
        return emitError("expected integer column number in FileLineColLoc");
      auto column = getToken().getUnsignedIntegerValue();
      if (!column.hasValue())
        return emitError("expected integer column number in FileLineColLoc");
      consumeToken(Token::integer);

      auto file = UniquedFilename::get(str, ctx);
      *loc = FileLineColLoc::get(file, line.getValue(), column.getValue(), ctx);
      return ParseSuccess;
    }

    // Otherwise, this is a NameLoc.
    *loc = NameLoc::get(Identifier::get(str, ctx), ctx);
    return ParseSuccess;
  }

  // Check for a 'unknown' for an unknown location.
  if (getToken().is(Token::bare_identifier) &&
      getToken().getSpelling() == "unknown") {
    consumeToken(Token::bare_identifier);
    *loc = UnknownLoc::get(ctx);
    return ParseSuccess;
  }

  // If the token is 'fused', then this is a fused location.
  if (getToken().is(Token::bare_identifier) &&
      getToken().getSpelling() == "fused") {
    consumeToken(Token::bare_identifier);

    // Try to parse the optional metadata.
    Attribute metadata;
    if (consumeIf(Token::less)) {
      metadata = parseAttribute();
      if (!metadata)
        return emitError("expected valid attribute metadata");
      // Parse the '>' token.
      if (parseToken(Token::greater,
                     "expected '>' after fused location metadata"))
        return ParseFailure;
    }

    // Parse the '['.
    if (parseToken(Token::l_square, "expected '[' in fused location"))
      return ParseFailure;

    // Parse the internal locations.
    llvm::SmallVector<Location, 4> locations;
    do {
      llvm::Optional<Location> newLoc;
      if (parseLocationInstance(&newLoc))
        return ParseFailure;
      locations.push_back(*newLoc);

      // Parse the ','.
    } while (consumeIf(Token::comma));

    // Parse the ']'.
    if (parseToken(Token::r_square, "expected ']' in fused location"))
      return ParseFailure;

    // Return the fused location.
    if (metadata)
      *loc = FusedLoc::get(locations, metadata, getContext());
    else
      *loc = FusedLoc::get(locations, ctx);
    return ParseSuccess;
  }

  // Check for the 'callsite' signifying a callsite location.
  if (getToken().is(Token::bare_identifier) &&
      getToken().getSpelling() == "callsite") {
    consumeToken(Token::bare_identifier);

    // Parse the '('.
    if (parseToken(Token::l_paren, "expected '(' in callsite location"))
      return ParseFailure;

    // Parse the callee location.
    llvm::Optional<Location> calleeLoc;
    if (parseLocationInstance(&calleeLoc))
      return ParseFailure;

    // Parse the 'at'.
    if (getToken().isNot(Token::bare_identifier) ||
        getToken().getSpelling() != "at")
      return emitError("expected 'at' in callsite location");
    consumeToken(Token::bare_identifier);

    // Parse the caller location.
    llvm::Optional<Location> callerLoc;
    if (parseLocationInstance(&callerLoc))
      return ParseFailure;

    // Parse the ')'.
    if (parseToken(Token::r_paren, "expected ')' in callsite location"))
      return ParseFailure;

    // Return the callsite location.
    *loc = CallSiteLoc::get(*calleeLoc, *callerLoc, ctx);
    return ParseSuccess;
  }

  return emitError("expected location instance");
}

/// Attribute dictionary.
///
///   attribute-dict ::= `{` `}`
///                    | `{` attribute-entry (`,` attribute-entry)* `}`
///   attribute-entry ::= bare-id `:` attribute-value
///
ParseResult
Parser::parseAttributeDict(SmallVectorImpl<NamedAttribute> &attributes) {
  if (!consumeIf(Token::l_brace))
    return ParseFailure;

  auto parseElt = [&]() -> ParseResult {
    // We allow keywords as attribute names.
    if (getToken().isNot(Token::bare_identifier, Token::inttype) &&
        !getToken().isKeyword())
      return emitError("expected attribute name");
    Identifier nameId = builder.getIdentifier(getTokenSpelling());
    consumeToken();

    if (parseToken(Token::colon, "expected ':' in attribute list"))
      return ParseFailure;

    auto attr = parseAttribute();
    if (!attr)
      return ParseFailure;

    attributes.push_back({nameId, attr});
    return ParseSuccess;
  };

  if (parseCommaSeparatedListUntil(Token::r_brace, parseElt))
    return ParseFailure;

  return ParseSuccess;
}

//===----------------------------------------------------------------------===//
// Polyhedral structures.
//===----------------------------------------------------------------------===//

/// Lower precedence ops (all at the same precedence level). LNoOp is false in
/// the boolean sense.
enum AffineLowPrecOp {
  /// Null value.
  LNoOp,
  Add,
  Sub
};

/// Higher precedence ops - all at the same precedence level. HNoOp is false
/// in the boolean sense.
enum AffineHighPrecOp {
  /// Null value.
  HNoOp,
  Mul,
  FloorDiv,
  CeilDiv,
  Mod
};

namespace {
/// This is a specialized parser for affine structures (affine maps, affine
/// expressions, and integer sets), maintaining the state transient to their
/// bodies.
class AffineParser : public Parser {
public:
  explicit AffineParser(ParserState &state) : Parser(state) {}

  AffineMap parseAffineMapInline();
  AffineMap parseAffineMapRange(unsigned numDims, unsigned numSymbols);
  IntegerSet parseIntegerSetInline();
  ParseResult parseAffineMapOrIntegerSetInline(AffineMap &map, IntegerSet &set);
  IntegerSet parseIntegerSetConstraints(unsigned numDims, unsigned numSymbols);

private:
  // Binary affine op parsing.
  AffineLowPrecOp consumeIfLowPrecOp();
  AffineHighPrecOp consumeIfHighPrecOp();

  // Identifier lists for polyhedral structures.
  ParseResult parseDimIdList(unsigned &numDims);
  ParseResult parseSymbolIdList(unsigned &numSymbols);
  ParseResult parseDimAndOptionalSymbolIdList(unsigned &numDims,
                                              unsigned &numSymbols);
  ParseResult parseIdentifierDefinition(AffineExpr idExpr);

  AffineExpr parseAffineExpr();
  AffineExpr parseParentheticalExpr();
  AffineExpr parseNegateExpression(AffineExpr lhs);
  AffineExpr parseIntegerExpr();
  AffineExpr parseBareIdExpr();

  AffineExpr getAffineBinaryOpExpr(AffineHighPrecOp op, AffineExpr lhs,
                                   AffineExpr rhs, SMLoc opLoc);
  AffineExpr getAffineBinaryOpExpr(AffineLowPrecOp op, AffineExpr lhs,
                                   AffineExpr rhs);
  AffineExpr parseAffineOperandExpr(AffineExpr lhs);
  AffineExpr parseAffineLowPrecOpExpr(AffineExpr llhs, AffineLowPrecOp llhsOp);
  AffineExpr parseAffineHighPrecOpExpr(AffineExpr llhs, AffineHighPrecOp llhsOp,
                                       SMLoc llhsOpLoc);
  AffineExpr parseAffineConstraint(bool *isEq);

private:
  SmallVector<std::pair<StringRef, AffineExpr>, 4> dimsAndSymbols;
};
} // end anonymous namespace

/// Create an affine binary high precedence op expression (mul's, div's, mod).
/// opLoc is the location of the op token to be used to report errors
/// for non-conforming expressions.
AffineExpr AffineParser::getAffineBinaryOpExpr(AffineHighPrecOp op,
                                               AffineExpr lhs, AffineExpr rhs,
                                               SMLoc opLoc) {
  // TODO: make the error location info accurate.
  switch (op) {
  case Mul:
    if (!lhs.isSymbolicOrConstant() && !rhs.isSymbolicOrConstant()) {
      emitError(opLoc, "non-affine expression: at least one of the multiply "
                       "operands has to be either a constant or symbolic");
      return nullptr;
    }
    return lhs * rhs;
  case FloorDiv:
    if (!rhs.isSymbolicOrConstant()) {
      emitError(opLoc, "non-affine expression: right operand of floordiv "
                       "has to be either a constant or symbolic");
      return nullptr;
    }
    return lhs.floorDiv(rhs);
  case CeilDiv:
    if (!rhs.isSymbolicOrConstant()) {
      emitError(opLoc, "non-affine expression: right operand of ceildiv "
                       "has to be either a constant or symbolic");
      return nullptr;
    }
    return lhs.ceilDiv(rhs);
  case Mod:
    if (!rhs.isSymbolicOrConstant()) {
      emitError(opLoc, "non-affine expression: right operand of mod "
                       "has to be either a constant or symbolic");
      return nullptr;
    }
    return lhs % rhs;
  case HNoOp:
    llvm_unreachable("can't create affine expression for null high prec op");
    return nullptr;
  }
}

/// Create an affine binary low precedence op expression (add, sub).
AffineExpr AffineParser::getAffineBinaryOpExpr(AffineLowPrecOp op,
                                               AffineExpr lhs, AffineExpr rhs) {
  switch (op) {
  case AffineLowPrecOp::Add:
    return lhs + rhs;
  case AffineLowPrecOp::Sub:
    return lhs - rhs;
  case AffineLowPrecOp::LNoOp:
    llvm_unreachable("can't create affine expression for null low prec op");
    return nullptr;
  }
}

/// Consume this token if it is a lower precedence affine op (there are only
/// two precedence levels).
AffineLowPrecOp AffineParser::consumeIfLowPrecOp() {
  switch (getToken().getKind()) {
  case Token::plus:
    consumeToken(Token::plus);
    return AffineLowPrecOp::Add;
  case Token::minus:
    consumeToken(Token::minus);
    return AffineLowPrecOp::Sub;
  default:
    return AffineLowPrecOp::LNoOp;
  }
}

/// Consume this token if it is a higher precedence affine op (there are only
/// two precedence levels)
AffineHighPrecOp AffineParser::consumeIfHighPrecOp() {
  switch (getToken().getKind()) {
  case Token::star:
    consumeToken(Token::star);
    return Mul;
  case Token::kw_floordiv:
    consumeToken(Token::kw_floordiv);
    return FloorDiv;
  case Token::kw_ceildiv:
    consumeToken(Token::kw_ceildiv);
    return CeilDiv;
  case Token::kw_mod:
    consumeToken(Token::kw_mod);
    return Mod;
  default:
    return HNoOp;
  }
}

/// Parse a high precedence op expression list: mul, div, and mod are high
/// precedence binary ops, i.e., parse a
///   expr_1 op_1 expr_2 op_2 ... expr_n
/// where op_1, op_2 are all a AffineHighPrecOp (mul, div, mod).
/// All affine binary ops are left associative.
/// Given llhs, returns (llhs llhsOp lhs) op rhs, or (lhs op rhs) if llhs is
/// null. If no rhs can be found, returns (llhs llhsOp lhs) or lhs if llhs is
/// null. llhsOpLoc is the location of the llhsOp token that will be used to
/// report an error for non-conforming expressions.
AffineExpr AffineParser::parseAffineHighPrecOpExpr(AffineExpr llhs,
                                                   AffineHighPrecOp llhsOp,
                                                   SMLoc llhsOpLoc) {
  AffineExpr lhs = parseAffineOperandExpr(llhs);
  if (!lhs)
    return nullptr;

  // Found an LHS. Parse the remaining expression.
  auto opLoc = getToken().getLoc();
  if (AffineHighPrecOp op = consumeIfHighPrecOp()) {
    if (llhs) {
      AffineExpr expr = getAffineBinaryOpExpr(llhsOp, llhs, lhs, opLoc);
      if (!expr)
        return nullptr;
      return parseAffineHighPrecOpExpr(expr, op, opLoc);
    }
    // No LLHS, get RHS
    return parseAffineHighPrecOpExpr(lhs, op, opLoc);
  }

  // This is the last operand in this expression.
  if (llhs)
    return getAffineBinaryOpExpr(llhsOp, llhs, lhs, llhsOpLoc);

  // No llhs, 'lhs' itself is the expression.
  return lhs;
}

/// Parse an affine expression inside parentheses.
///
///   affine-expr ::= `(` affine-expr `)`
AffineExpr AffineParser::parseParentheticalExpr() {
  if (parseToken(Token::l_paren, "expected '('"))
    return nullptr;
  if (getToken().is(Token::r_paren))
    return (emitError("no expression inside parentheses"), nullptr);

  auto expr = parseAffineExpr();
  if (!expr)
    return nullptr;
  if (parseToken(Token::r_paren, "expected ')'"))
    return nullptr;

  return expr;
}

/// Parse the negation expression.
///
///   affine-expr ::= `-` affine-expr
AffineExpr AffineParser::parseNegateExpression(AffineExpr lhs) {
  if (parseToken(Token::minus, "expected '-'"))
    return nullptr;

  AffineExpr operand = parseAffineOperandExpr(lhs);
  // Since negation has the highest precedence of all ops (including high
  // precedence ops) but lower than parentheses, we are only going to use
  // parseAffineOperandExpr instead of parseAffineExpr here.
  if (!operand)
    // Extra error message although parseAffineOperandExpr would have
    // complained. Leads to a better diagnostic.
    return (emitError("missing operand of negation"), nullptr);
  return (-1) * operand;
}

/// Parse a bare id that may appear in an affine expression.
///
///   affine-expr ::= bare-id
AffineExpr AffineParser::parseBareIdExpr() {
  if (getToken().isNot(Token::bare_identifier))
    return (emitError("expected bare identifier"), nullptr);

  StringRef sRef = getTokenSpelling();
  for (auto entry : dimsAndSymbols) {
    if (entry.first == sRef) {
      consumeToken(Token::bare_identifier);
      return entry.second;
    }
  }

  return (emitError("use of undeclared identifier"), nullptr);
}

/// Parse a positive integral constant appearing in an affine expression.
///
///   affine-expr ::= integer-literal
AffineExpr AffineParser::parseIntegerExpr() {
  auto val = getToken().getUInt64IntegerValue();
  if (!val.hasValue() || (int64_t)val.getValue() < 0)
    return (emitError("constant too large for index"), nullptr);

  consumeToken(Token::integer);
  return builder.getAffineConstantExpr((int64_t)val.getValue());
}

/// Parses an expression that can be a valid operand of an affine expression.
/// lhs: if non-null, lhs is an affine expression that is the lhs of a binary
/// operator, the rhs of which is being parsed. This is used to determine
/// whether an error should be emitted for a missing right operand.
//  Eg: for an expression without parentheses (like i + j + k + l), each
//  of the four identifiers is an operand. For i + j*k + l, j*k is not an
//  operand expression, it's an op expression and will be parsed via
//  parseAffineHighPrecOpExpression(). However, for i + (j*k) + -l, (j*k) and
//  -l are valid operands that will be parsed by this function.
AffineExpr AffineParser::parseAffineOperandExpr(AffineExpr lhs) {
  switch (getToken().getKind()) {
  case Token::bare_identifier:
    return parseBareIdExpr();
  case Token::integer:
    return parseIntegerExpr();
  case Token::l_paren:
    return parseParentheticalExpr();
  case Token::minus:
    return parseNegateExpression(lhs);
  case Token::kw_ceildiv:
  case Token::kw_floordiv:
  case Token::kw_mod:
  case Token::plus:
  case Token::star:
    if (lhs)
      emitError("missing right operand of binary operator");
    else
      emitError("missing left operand of binary operator");
    return nullptr;
  default:
    if (lhs)
      emitError("missing right operand of binary operator");
    else
      emitError("expected affine expression");
    return nullptr;
  }
}

/// Parse affine expressions that are bare-id's, integer constants,
/// parenthetical affine expressions, and affine op expressions that are a
/// composition of those.
///
/// All binary op's associate from left to right.
///
/// {add, sub} have lower precedence than {mul, div, and mod}.
///
/// Add, sub'are themselves at the same precedence level. Mul, floordiv,
/// ceildiv, and mod are at the same higher precedence level. Negation has
/// higher precedence than any binary op.
///
/// llhs: the affine expression appearing on the left of the one being parsed.
/// This function will return ((llhs llhsOp lhs) op rhs) if llhs is non null,
/// and lhs op rhs otherwise; if there is no rhs, llhs llhsOp lhs is returned
/// if llhs is non-null; otherwise lhs is returned. This is to deal with left
/// associativity.
///
/// Eg: when the expression is e1 + e2*e3 + e4, with e1 as llhs, this function
/// will return the affine expr equivalent of (e1 + (e2*e3)) + e4, where
/// (e2*e3) will be parsed using parseAffineHighPrecOpExpr().
AffineExpr AffineParser::parseAffineLowPrecOpExpr(AffineExpr llhs,
                                                  AffineLowPrecOp llhsOp) {
  AffineExpr lhs;
  if (!(lhs = parseAffineOperandExpr(llhs)))
    return nullptr;

  // Found an LHS. Deal with the ops.
  if (AffineLowPrecOp lOp = consumeIfLowPrecOp()) {
    if (llhs) {
      AffineExpr sum = getAffineBinaryOpExpr(llhsOp, llhs, lhs);
      return parseAffineLowPrecOpExpr(sum, lOp);
    }
    // No LLHS, get RHS and form the expression.
    return parseAffineLowPrecOpExpr(lhs, lOp);
  }
  auto opLoc = getToken().getLoc();
  if (AffineHighPrecOp hOp = consumeIfHighPrecOp()) {
    // We have a higher precedence op here. Get the rhs operand for the llhs
    // through parseAffineHighPrecOpExpr.
    AffineExpr highRes = parseAffineHighPrecOpExpr(lhs, hOp, opLoc);
    if (!highRes)
      return nullptr;

    // If llhs is null, the product forms the first operand of the yet to be
    // found expression. If non-null, the op to associate with llhs is llhsOp.
    AffineExpr expr =
        llhs ? getAffineBinaryOpExpr(llhsOp, llhs, highRes) : highRes;

    // Recurse for subsequent low prec op's after the affine high prec op
    // expression.
    if (AffineLowPrecOp nextOp = consumeIfLowPrecOp())
      return parseAffineLowPrecOpExpr(expr, nextOp);
    return expr;
  }
  // Last operand in the expression list.
  if (llhs)
    return getAffineBinaryOpExpr(llhsOp, llhs, lhs);
  // No llhs, 'lhs' itself is the expression.
  return lhs;
}

/// Parse an affine expression.
///  affine-expr ::= `(` affine-expr `)`
///                | `-` affine-expr
///                | affine-expr `+` affine-expr
///                | affine-expr `-` affine-expr
///                | affine-expr `*` affine-expr
///                | affine-expr `floordiv` affine-expr
///                | affine-expr `ceildiv` affine-expr
///                | affine-expr `mod` affine-expr
///                | bare-id
///                | integer-literal
///
/// Additional conditions are checked depending on the production. For eg.,
/// one of the operands for `*` has to be either constant/symbolic; the second
/// operand for floordiv, ceildiv, and mod has to be a positive integer.
AffineExpr AffineParser::parseAffineExpr() {
  return parseAffineLowPrecOpExpr(nullptr, AffineLowPrecOp::LNoOp);
}

/// Parse a dim or symbol from the lists appearing before the actual
/// expressions of the affine map. Update our state to store the
/// dimensional/symbolic identifier.
ParseResult AffineParser::parseIdentifierDefinition(AffineExpr idExpr) {
  if (getToken().isNot(Token::bare_identifier))
    return emitError("expected bare identifier");

  auto name = getTokenSpelling();
  for (auto entry : dimsAndSymbols) {
    if (entry.first == name)
      return emitError("redefinition of identifier '" + Twine(name) + "'");
  }
  consumeToken(Token::bare_identifier);

  dimsAndSymbols.push_back({name, idExpr});
  return ParseSuccess;
}

/// Parse the list of dimensional identifiers to an affine map.
ParseResult AffineParser::parseDimIdList(unsigned &numDims) {
  if (parseToken(Token::l_paren,
                 "expected '(' at start of dimensional identifiers list")) {
    return ParseFailure;
  }

  auto parseElt = [&]() -> ParseResult {
    auto dimension = getAffineDimExpr(numDims++, getContext());
    return parseIdentifierDefinition(dimension);
  };
  return parseCommaSeparatedListUntil(Token::r_paren, parseElt);
}

/// Parse the list of symbolic identifiers to an affine map.
ParseResult AffineParser::parseSymbolIdList(unsigned &numSymbols) {
  consumeToken(Token::l_square);
  auto parseElt = [&]() -> ParseResult {
    auto symbol = getAffineSymbolExpr(numSymbols++, getContext());
    return parseIdentifierDefinition(symbol);
  };
  return parseCommaSeparatedListUntil(Token::r_square, parseElt);
}

/// Parse the list of symbolic identifiers to an affine map.
ParseResult
AffineParser::parseDimAndOptionalSymbolIdList(unsigned &numDims,
                                              unsigned &numSymbols) {
  if (parseDimIdList(numDims)) {
    return ParseResult::ParseFailure;
  }
  if (!getToken().is(Token::l_square)) {
    numSymbols = 0;
    return ParseResult::ParseSuccess;
  }
  return parseSymbolIdList(numSymbols);
}

/// Parses an affine map definition inline.
///
///  affine-map-inline ::= dim-and-symbol-id-lists `->` multi-dim-affine-expr
///                        (`size` `(` dim-size (`,` dim-size)* `)`)?
///  dim-size ::= affine-expr | `min` `(` affine-expr ( `,` affine-expr)+ `)`
///
///  multi-dim-affine-expr ::= `(` affine-expr (`,` affine-expr)* `)
///
AffineMap AffineParser::parseAffineMapInline() {
  unsigned numDims = 0, numSymbols = 0;

  // List of dimensional and optional symbol identifiers.
  if (parseDimAndOptionalSymbolIdList(numDims, numSymbols)) {
    return AffineMap();
  }

  if (parseToken(Token::arrow, "expected '->' or '['")) {
    return AffineMap();
  }

  // Parse the affine map.
  return parseAffineMapRange(numDims, numSymbols);
}

/// Parses an integer set definition inline.
///
///  integer-set-inline
///                ::= dim-and-symbol-id-lists `:`
///                affine-constraint-conjunction
///  affine-constraint-conjunction ::= /*empty*/
///                                 | affine-constraint (`,`
///                                 affine-constraint)*
///
IntegerSet AffineParser::parseIntegerSetInline() {
  unsigned numDims = 0, numSymbols = 0;

  // List of dimensional and optional symbol identifiers.
  if (parseDimAndOptionalSymbolIdList(numDims, numSymbols)) {
    return IntegerSet();
  }

  if (parseToken(Token::colon, "expected ':' or '['")) {
    return IntegerSet();
  }

  return parseIntegerSetConstraints(numDims, numSymbols);
}

/// Parses an ambiguous affine map or integer set definition inline.
ParseResult AffineParser::parseAffineMapOrIntegerSetInline(AffineMap &map,
                                                           IntegerSet &set) {
  unsigned numDims = 0, numSymbols = 0;

  // List of dimensional and optional symbol identifiers.
  if (parseDimAndOptionalSymbolIdList(numDims, numSymbols)) {
    return ParseResult::ParseFailure;
  }

  // This is needed for parsing attributes as we wouldn't know whether we would
  // be parsing an integer set attribute or an affine map attribute.
  bool isArrow = getToken().is(Token::arrow);
  bool isColon = getToken().is(Token::colon);
  if (!isArrow && !isColon) {
    return emitError("expected '->' or ':'");
  } else if (isArrow) {
    parseToken(Token::arrow, "expected '->' or '['");
    map = parseAffineMapRange(numDims, numSymbols);
    return map ? ParseSuccess : ParseFailure;
  } else if (parseToken(Token::colon, "expected ':' or '['")) {
    return ParseFailure;
  }

  if ((set = parseIntegerSetConstraints(numDims, numSymbols)))
    return ParseSuccess;

  return ParseFailure;
}

/// Parse the range and sizes affine map definition inline.
///
///  affine-map-inline ::= dim-and-symbol-id-lists `->` multi-dim-affine-expr
///                        (`size` `(` dim-size (`,` dim-size)* `)`)?
///  dim-size ::= affine-expr | `min` `(` affine-expr ( `,` affine-expr)+ `)`
///
///  multi-dim-affine-expr ::= `(` affine-expr (`,` affine-expr)* `)
AffineMap AffineParser::parseAffineMapRange(unsigned numDims,
                                            unsigned numSymbols) {
  parseToken(Token::l_paren, "expected '(' at start of affine map range");

  SmallVector<AffineExpr, 4> exprs;
  auto parseElt = [&]() -> ParseResult {
    auto elt = parseAffineExpr();
    ParseResult res = elt ? ParseSuccess : ParseFailure;
    exprs.push_back(elt);
    return res;
  };

  // Parse a multi-dimensional affine expression (a comma-separated list of
  // 1-d affine expressions); the list cannot be empty. Grammar:
  // multi-dim-affine-expr ::= `(` affine-expr (`,` affine-expr)* `)
  if (parseCommaSeparatedListUntil(Token::r_paren, parseElt, false))
    return AffineMap();

  // Parse optional range sizes.
  //  range-sizes ::= (`size` `(` dim-size (`,` dim-size)* `)`)?
  //  dim-size ::= affine-expr | `min` `(` affine-expr (`,` affine-expr)+ `)`
  // TODO(bondhugula): support for min of several affine expressions.
  // TODO: check if sizes are non-negative whenever they are constant.
  SmallVector<AffineExpr, 4> rangeSizes;
  if (consumeIf(Token::kw_size)) {
    // Location of the l_paren token (if it exists) for error reporting later.
    auto loc = getToken().getLoc();
    if (parseToken(Token::l_paren, "expected '(' at start of affine map range"))
      return AffineMap();

    auto parseRangeSize = [&]() -> ParseResult {
      auto loc = getToken().getLoc();
      auto elt = parseAffineExpr();
      if (!elt)
        return ParseFailure;

      if (!elt.isSymbolicOrConstant())
        return emitError(loc,
                         "size expressions cannot refer to dimension values");

      rangeSizes.push_back(elt);
      return ParseSuccess;
    };

    if (parseCommaSeparatedListUntil(Token::r_paren, parseRangeSize, false))
      return AffineMap();
    if (exprs.size() > rangeSizes.size())
      return (emitError(loc, "fewer range sizes than range expressions"),
              AffineMap());
    if (exprs.size() < rangeSizes.size())
      return (emitError(loc, "more range sizes than range expressions"),
              AffineMap());
  }

  // Parsed a valid affine map.
  return builder.getAffineMap(numDims, numSymbols, exprs, rangeSizes);
}

/// Parse a reference to an integer set.
///  integer-set ::= integer-set-id | integer-set-inline
///  integer-set-id ::= `#` suffix-id
///
IntegerSet Parser::parseIntegerSetReference() {
  if (getToken().isNot(Token::hash_identifier)) {
    // Try to parse inline integer set.
    return AffineParser(state).parseIntegerSetInline();
  }

  // Parse integer set identifier and verify that it exists.
  StringRef id = getTokenSpelling().drop_front();
  if (getState().integerSetDefinitions.count(id) > 0) {
    consumeToken(Token::hash_identifier);
    return getState().integerSetDefinitions[id];
  }

  // The id isn't among any of the recorded definitions.
  emitError("undefined integer set id '" + id + "'");
  return IntegerSet();
}

/// Parse a reference to an affine map.
///  affine-map ::= affine-map-id | affine-map-inline
///  affine-map-id ::= `#` suffix-id
///
AffineMap Parser::parseAffineMapReference() {
  if (getToken().isNot(Token::hash_identifier)) {
    // Try to parse inline affine map.
    return AffineParser(state).parseAffineMapInline();
  }

  // Parse affine map identifier and verify that it exists.
  StringRef id = getTokenSpelling().drop_front();
  if (getState().affineMapDefinitions.count(id) > 0) {
    consumeToken(Token::hash_identifier);
    return getState().affineMapDefinitions[id];
  }

  // The id isn't among any of the recorded definitions.
  emitError("undefined affine map id '" + id + "'");
  return AffineMap();
}

/// Parse an ambiguous reference to either and affine map or an integer set.
ParseResult Parser::parseAffineMapOrIntegerSetReference(AffineMap &map,
                                                        IntegerSet &set) {
  if (getToken().isNot(Token::hash_identifier)) {
    // Try to parse inline affine map.
    return AffineParser(state).parseAffineMapOrIntegerSetInline(map, set);
  }

  // Parse affine map / integer set identifier and verify that it exists.
  // Note that an id can't be in both affineMapDefinitions and
  // integerSetDefinitions since they use the same sigil '#'.
  StringRef id = getTokenSpelling().drop_front();
  if (getState().affineMapDefinitions.count(id) > 0) {
    consumeToken(Token::hash_identifier);
    map = getState().affineMapDefinitions[id];
    return ParseSuccess;
  }
  if (getState().integerSetDefinitions.count(id) > 0) {
    consumeToken(Token::hash_identifier);
    set = getState().integerSetDefinitions[id];
    return ParseSuccess;
  }

  // The id isn't among any of the recorded definitions.
  emitError("undefined affine map or integer set id '" + id + "'");

  return ParseFailure;
}

//===----------------------------------------------------------------------===//
// FunctionParser
//===----------------------------------------------------------------------===//

namespace {
/// This class contains parser state that is common across CFG and ML
/// functions, notably for dealing with operations and SSA values.
class FunctionParser : public Parser {
public:
  /// This builder intentionally shadows the builder in the base class, with a
  /// more specific builder type.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow-field"
  FuncBuilder builder;
#pragma clang diagnostic pop

  FunctionParser(ParserState &state, Function *function)
      : Parser(state), builder(function), function(function) {}

  ~FunctionParser();

  ParseResult parseFunctionBody(bool hadNamedArguments);

  /// Parse a single operation successor and it's operand list.
  bool parseSuccessorAndUseList(Block *&dest,
                                SmallVectorImpl<Value *> &operands);

  /// Parse a comma-separated list of operation successors in brackets.
  ParseResult
  parseSuccessors(SmallVectorImpl<Block *> &destinations,
                  SmallVectorImpl<SmallVector<Value *, 4>> &operands);

  /// After the function is finished parsing, this function checks to see if
  /// there are any remaining issues.
  ParseResult finalizeFunction(SMLoc loc);

  /// This represents a use of an SSA value in the program.  The first two
  /// entries in the tuple are the name and result number of a reference.  The
  /// third is the location of the reference, which is used in case this ends
  /// up being a use of an undefined value.
  struct SSAUseInfo {
    StringRef name;  // Value name, e.g. %42 or %abc
    unsigned number; // Number, specified with #12
    SMLoc loc;       // Location of first definition or use.
  };

  /// Given a reference to an SSA value and its type, return a reference. This
  /// returns null on failure.
  Value *resolveSSAUse(SSAUseInfo useInfo, Type type);

  /// Register a definition of a value with the symbol table.
  ParseResult addDefinition(SSAUseInfo useInfo, Value *value);

  // SSA parsing productions.
  ParseResult parseSSAUse(SSAUseInfo &result);
  ParseResult parseOptionalSSAUseList(SmallVectorImpl<SSAUseInfo> &results);

  template <typename ResultType>
  ResultType parseSSADefOrUseAndType(
      const std::function<ResultType(SSAUseInfo, Type)> &action);

  Value *parseSSAUseAndType() {
    return parseSSADefOrUseAndType<Value *>(
        [&](SSAUseInfo useInfo, Type type) -> Value * {
          return resolveSSAUse(useInfo, type);
        });
  }

  template <typename ValueTy>
  ParseResult
  parseOptionalSSAUseAndTypeList(SmallVectorImpl<ValueTy *> &results);

  // Block references.

  ParseResult
  parseOperationRegion(SmallVectorImpl<Block *> &results,
                       ArrayRef<std::pair<SSAUseInfo, Type>> entryArguments);
  ParseResult parseRegionBody(SmallVectorImpl<Block *> &results);
  ParseResult parseBlock(Block *&block);
  ParseResult parseBlockBody(Block *block);

  ParseResult
  parseOptionalBlockArgList(SmallVectorImpl<BlockArgument *> &results,
                            Block *owner);

  /// Cleans up the memory for allocated blocks when a parser error occurs.
  void cleanupInvalidBlocks(ArrayRef<Block *> invalidBlocks) {
    // Add the referenced blocks to the function so that they can be properly
    // cleaned up when the function is destroyed.
    for (auto *block : invalidBlocks)
      function->push_back(block);
  }

  /// Get the block with the specified name, creating it if it doesn't
  /// already exist.  The location specified is the point of use, which allows
  /// us to diagnose references to blocks that are not defined precisely.
  Block *getBlockNamed(StringRef name, SMLoc loc);

  // Define the block with the specified name. Returns the Block* or
  // nullptr in the case of redefinition.
  Block *defineBlockNamed(StringRef name, SMLoc loc, Block *existing);

  // Operations
  ParseResult parseOperation();
  Instruction *parseGenericOperation();
  Instruction *parseCustomOperation();

  ParseResult parseInstructions(Block *block);

private:
  Function *function;

  // This keeps track of the block names as well as the location of the first
  // reference, used to diagnose invalid block references and memoize them.
  llvm::StringMap<std::pair<Block *, SMLoc>> blocksByName;
  DenseMap<Block *, SMLoc> forwardRef;

  /// This keeps track of all of the SSA values we are tracking, indexed by
  /// their name.  This has one entry per result number.
  llvm::StringMap<SmallVector<std::pair<Value *, SMLoc>, 1>> values;

  /// These are all of the placeholders we've made along with the location of
  /// their first reference, to allow checking for use of undefined values.
  DenseMap<Value *, SMLoc> forwardReferencePlaceholders;

  Value *createForwardReferencePlaceholder(SMLoc loc, Type type);

  /// Return true if this is a forward reference.
  bool isForwardReferencePlaceholder(Value *value) {
    return forwardReferencePlaceholders.count(value);
  }
};
} // end anonymous namespace

ParseResult FunctionParser::parseFunctionBody(bool hadNamedArguments) {
  auto braceLoc = getToken().getLoc();
  if (parseToken(Token::l_brace, "expected '{' in function"))
    return ParseFailure;

  // Make sure we have at least one block.
  if (getToken().is(Token::r_brace))
    return emitError("function must have a body");

  // If we had named arguments, then we don't allow a block name.
  if (hadNamedArguments) {
    if (getToken().is(Token::caret_identifier))
      return emitError("invalid block name in function with named arguments");
  }

  // The first block is already created and should be filled in.
  auto firstBlock = &function->front();

  // Parse the first block.
  if (parseBlock(firstBlock))
    return ParseFailure;

  // Parse the remaining list of blocks.
  SmallVector<Block *, 16> blocks;
  if (parseRegionBody(blocks))
    return ParseFailure;
  function->getBlocks().insert(function->end(), blocks.begin(), blocks.end());

  // Verify that all referenced blocks were defined.
  if (!forwardRef.empty()) {
    SmallVector<std::pair<const char *, Block *>, 4> errors;
    // Iteration over the map isn't deterministic, so sort by source location.
    for (auto entry : forwardRef) {
      errors.push_back({entry.second.getPointer(), entry.first});
      cleanupInvalidBlocks(entry.first);
    }
    llvm::array_pod_sort(errors.begin(), errors.end());

    for (auto entry : errors) {
      auto loc = SMLoc::getFromPointer(entry.first);
      emitError(loc, "reference to an undefined block");
    }
    return ParseFailure;
  }

  return finalizeFunction(braceLoc);
}

/// Block list.
///
///   block-list ::= '{' block-list-body
///
ParseResult FunctionParser::parseOperationRegion(
    SmallVectorImpl<Block *> &results,
    ArrayRef<std::pair<FunctionParser::SSAUseInfo, Type>> entryArguments) {
  // Parse the '{'.
  if (parseToken(Token::l_brace, "expected '{' to begin a region"))
    return ParseFailure;

  // Check for an empty region.
  if (entryArguments.empty() && consumeIf(Token::r_brace))
    return ParseSuccess;
  Block *currentBlock = builder.getInsertionBlock();

  // Parse the first block directly to allow for it to be unnamed.
  Block *block = new Block();

  // Add arguments to the entry block.
  for (auto &placeholderArgPair : entryArguments)
    if (addDefinition(placeholderArgPair.first,
                      block->addArgument(placeholderArgPair.second))) {
      delete block;
      return ParseFailure;
    }

  if (parseBlock(block)) {
    delete block;
    return ParseFailure;
  }

  // Verify that no other arguments were parsed.
  if (!entryArguments.empty() &&
      block->getNumArguments() > entryArguments.size()) {
    delete block;
    return emitError("entry block arguments were already defined");
  }

  // Parse the rest of the region.
  results.push_back(block);
  if (parseRegionBody(results))
    return ParseFailure;

  // Reset insertion point to the current block.
  builder.setInsertionPointToEnd(currentBlock);
  return ParseSuccess;
}

/// Region.
///
///   region-body ::= block* '}'
///
ParseResult FunctionParser::parseRegionBody(SmallVectorImpl<Block *> &results) {
  // Parse the list of blocks.
  while (!consumeIf(Token::r_brace)) {
    Block *newBlock = nullptr;
    if (parseBlock(newBlock)) {
      cleanupInvalidBlocks(results);
      return ParseFailure;
    }
    results.push_back(newBlock);
  }
  return ParseSuccess;
}

/// Block declaration.
///
///   block ::= block-label? instruction* terminator-inst
///   block-label    ::= block-id block-arg-list? `:`
///   block-id       ::= caret-id
///   block-arg-list ::= `(` ssa-id-and-type-list? `)`
///
ParseResult FunctionParser::parseBlock(Block *&block) {
  // The first block for a function is already created.
  if (block) {
    // The name for a first block is optional.
    if (getToken().isNot(Token::caret_identifier))
      return parseBlockBody(block);
  }

  SMLoc nameLoc = getToken().getLoc();
  auto name = getTokenSpelling();
  if (parseToken(Token::caret_identifier, "expected block name"))
    return ParseFailure;

  block = defineBlockNamed(name, nameLoc, block);

  // Fail if redefinition.
  if (!block)
    return emitError(nameLoc, "redefinition of block '" + name.str() + "'");

  // If an argument list is present, parse it.
  if (consumeIf(Token::l_paren)) {
    SmallVector<BlockArgument *, 8> bbArgs;
    if (parseOptionalBlockArgList(bbArgs, block) ||
        parseToken(Token::r_paren, "expected ')' to end argument list"))
      return ParseFailure;
  }

  if (parseToken(Token::colon, "expected ':' after block name"))
    return ParseFailure;

  return parseBlockBody(block);
}

ParseResult FunctionParser::parseBlockBody(Block *block) {

  // Set the insertion point to the block we want to insert new operations
  // into.
  builder.setInsertionPointToEnd(block);

  // Parse the list of operations that make up the body of the block.
  while (getToken().isNot(Token::caret_identifier, Token::r_brace)) {
    switch (getToken().getKind()) {
    default:
      if (parseOperation())
        return ParseFailure;
      break;
    }
  }

  return ParseSuccess;
}

/// Create and remember a new placeholder for a forward reference.
Value *FunctionParser::createForwardReferencePlaceholder(SMLoc loc, Type type) {
  // Forward references are always created as instructions, even in ML
  // functions, because we just need something with a def/use chain.
  //
  // We create these placeholders as having an empty name, which we know
  // cannot be created through normal user input, allowing us to distinguish
  // them.
  auto name = OperationName("placeholder", getContext());
  auto *inst = Instruction::create(
      getEncodedSourceLocation(loc), name, /*operands=*/{}, type,
      /*attributes=*/llvm::None, /*successors=*/{}, /*numRegions=*/0,
      /*resizableOperandList=*/false, getContext());
  forwardReferencePlaceholders[inst->getResult(0)] = loc;
  return inst->getResult(0);
}

/// Given an unbound reference to an SSA value and its type, return the value
/// it specifies.  This returns null on failure.
Value *FunctionParser::resolveSSAUse(SSAUseInfo useInfo, Type type) {
  auto &entries = values[useInfo.name];

  // If we have already seen a value of this name, return it.
  if (useInfo.number < entries.size() && entries[useInfo.number].first) {
    auto *result = entries[useInfo.number].first;
    // Check that the type matches the other uses.
    if (result->getType() == type)
      return result;

    emitError(useInfo.loc, "use of value '" + useInfo.name.str() +
                               "' expects different type than prior uses");
    emitError(entries[useInfo.number].second, "prior use here");
    return nullptr;
  }

  // Make sure we have enough slots for this.
  if (entries.size() <= useInfo.number)
    entries.resize(useInfo.number + 1);

  // If the value has already been defined and this is an overly large result
  // number, diagnose that.
  if (entries[0].first && !isForwardReferencePlaceholder(entries[0].first))
    return (emitError(useInfo.loc, "reference to invalid result number"),
            nullptr);

  // Otherwise, this is a forward reference.  Create a placeholder and remember
  // that we did so.
  auto *result = createForwardReferencePlaceholder(useInfo.loc, type);
  entries[useInfo.number].first = result;
  entries[useInfo.number].second = useInfo.loc;
  return result;
}

/// After the function is finished parsing, this function checks to see if
/// there are any remaining issues.
ParseResult FunctionParser::finalizeFunction(SMLoc loc) {
  // Check for any forward references that are left.  If we find any, error
  // out.
  if (!forwardReferencePlaceholders.empty()) {
    SmallVector<std::pair<const char *, Value *>, 4> errors;
    // Iteration over the map isn't deterministic, so sort by source location.
    for (auto entry : forwardReferencePlaceholders)
      errors.push_back({entry.second.getPointer(), entry.first});
    llvm::array_pod_sort(errors.begin(), errors.end());

    for (auto entry : errors) {
      auto loc = SMLoc::getFromPointer(entry.first);
      emitError(loc, "use of undeclared SSA value name");
    }
    return ParseFailure;
  }

  return ParseSuccess;
}

FunctionParser::~FunctionParser() {
  for (auto &fwd : forwardReferencePlaceholders) {
    // Drop all uses of undefined forward declared reference and destroy
    // defining instruction.
    fwd.first->dropAllUses();
    fwd.first->getDefiningInst()->destroy();
  }
}

/// Register a definition of a value with the symbol table.
ParseResult FunctionParser::addDefinition(SSAUseInfo useInfo, Value *value) {
  auto &entries = values[useInfo.name];

  // Make sure there is a slot for this value.
  if (entries.size() <= useInfo.number)
    entries.resize(useInfo.number + 1);

  // If we already have an entry for this, check to see if it was a definition
  // or a forward reference.
  if (auto *existing = entries[useInfo.number].first) {
    if (!isForwardReferencePlaceholder(existing)) {
      emitError(useInfo.loc,
                "redefinition of SSA value '" + useInfo.name + "'");
      return emitError(entries[useInfo.number].second,
                       "previously defined here");
    }

    // If it was a forward reference, update everything that used it to use
    // the actual definition instead, delete the forward ref, and remove it
    // from our set of forward references we track.
    existing->replaceAllUsesWith(value);
    existing->getDefiningInst()->destroy();
    forwardReferencePlaceholders.erase(existing);
  }

  entries[useInfo.number].first = value;
  entries[useInfo.number].second = useInfo.loc;
  return ParseSuccess;
}

/// Parse a SSA operand for an instruction or instruction.
///
///   ssa-use ::= ssa-id
///
ParseResult FunctionParser::parseSSAUse(SSAUseInfo &result) {
  result.name = getTokenSpelling();
  result.number = 0;
  result.loc = getToken().getLoc();
  if (parseToken(Token::percent_identifier, "expected SSA operand"))
    return ParseFailure;

  // If we have an affine map ID, it is a result number.
  if (getToken().is(Token::hash_identifier)) {
    if (auto value = getToken().getHashIdentifierNumber())
      result.number = value.getValue();
    else
      return emitError("invalid SSA value result number");
    consumeToken(Token::hash_identifier);
  }

  return ParseSuccess;
}

/// Parse a (possibly empty) list of SSA operands.
///
///   ssa-use-list ::= ssa-use (`,` ssa-use)*
///   ssa-use-list-opt ::= ssa-use-list?
///
ParseResult
FunctionParser::parseOptionalSSAUseList(SmallVectorImpl<SSAUseInfo> &results) {
  if (getToken().isNot(Token::percent_identifier))
    return ParseSuccess;
  return parseCommaSeparatedList([&]() -> ParseResult {
    SSAUseInfo result;
    if (parseSSAUse(result))
      return ParseFailure;
    results.push_back(result);
    return ParseSuccess;
  });
}

/// Parse an SSA use with an associated type.
///
///   ssa-use-and-type ::= ssa-use `:` type
template <typename ResultType>
ResultType FunctionParser::parseSSADefOrUseAndType(
    const std::function<ResultType(SSAUseInfo, Type)> &action) {

  SSAUseInfo useInfo;
  if (parseSSAUse(useInfo) ||
      parseToken(Token::colon, "expected ':' and type for SSA operand"))
    return nullptr;

  auto type = parseType();
  if (!type)
    return nullptr;

  return action(useInfo, type);
}

/// Parse a (possibly empty) list of SSA operands, followed by a colon, then
/// followed by a type list.
///
///   ssa-use-and-type-list
///     ::= ssa-use-list ':' type-list-no-parens
///
template <typename ValueTy>
ParseResult FunctionParser::parseOptionalSSAUseAndTypeList(
    SmallVectorImpl<ValueTy *> &results) {
  SmallVector<SSAUseInfo, 4> valueIDs;
  if (parseOptionalSSAUseList(valueIDs))
    return ParseFailure;

  // If there were no operands, then there is no colon or type lists.
  if (valueIDs.empty())
    return ParseSuccess;

  SmallVector<Type, 4> types;
  if (parseToken(Token::colon, "expected ':' in operand list") ||
      parseTypeListNoParens(types))
    return ParseFailure;

  if (valueIDs.size() != types.size())
    return emitError("expected " + Twine(valueIDs.size()) +
                     " types to match operand list");

  results.reserve(valueIDs.size());
  for (unsigned i = 0, e = valueIDs.size(); i != e; ++i) {
    if (auto *value = resolveSSAUse(valueIDs[i], types[i]))
      results.push_back(cast<ValueTy>(value));
    else
      return ParseFailure;
  }

  return ParseSuccess;
}

/// Get the block with the specified name, creating it if it doesn't already
/// exist.  The location specified is the point of use, which allows
/// us to diagnose references to blocks that are not defined precisely.
Block *FunctionParser::getBlockNamed(StringRef name, SMLoc loc) {
  auto &blockAndLoc = blocksByName[name];
  if (!blockAndLoc.first) {
    blockAndLoc.first = new Block();
    forwardRef[blockAndLoc.first] = loc;
    blockAndLoc.second = loc;
  }

  return blockAndLoc.first;
}

/// Define the block with the specified name. Returns the Block* or nullptr in
/// the case of redefinition.
Block *FunctionParser::defineBlockNamed(StringRef name, SMLoc loc,
                                        Block *existing) {
  auto &blockAndLoc = blocksByName[name];
  if (!blockAndLoc.first) {
    // If the caller provided a block, use it.  Otherwise create a new one.
    if (!existing)
      existing = new Block();
    blockAndLoc.first = existing;
    blockAndLoc.second = loc;
    return blockAndLoc.first;
  }

  // Forward declarations are removed once defined, so if we are defining a
  // existing block and it is not a forward declaration, then it is a
  // redeclaration.
  if (!forwardRef.erase(blockAndLoc.first))
    return nullptr;
  return blockAndLoc.first;
}

/// Parse a single operation successor and it's operand list.
///
///   successor ::= block-id branch-use-list?
///   branch-use-list ::= `(` ssa-use-list ':' type-list-no-parens `)`
///
bool FunctionParser::parseSuccessorAndUseList(
    Block *&dest, SmallVectorImpl<Value *> &operands) {
  // Verify branch is identifier and get the matching block.
  if (!getToken().is(Token::caret_identifier))
    return emitError("expected block name");
  dest = getBlockNamed(getTokenSpelling(), getToken().getLoc());
  consumeToken();

  // Handle optional arguments.
  if (consumeIf(Token::l_paren) &&
      (parseOptionalSSAUseAndTypeList(operands) ||
       parseToken(Token::r_paren, "expected ')' to close argument list"))) {
    return true;
  }

  return false;
}

/// Parse a comma-separated list of operation successors in brackets.
///
///   successor-list ::= `[` successor (`,` successor )* `]`
///
ParseResult FunctionParser::parseSuccessors(
    SmallVectorImpl<Block *> &destinations,
    SmallVectorImpl<SmallVector<Value *, 4>> &operands) {
  if (parseToken(Token::l_square, "expected '['"))
    return ParseFailure;

  auto parseElt = [this, &destinations, &operands]() {
    Block *dest;
    SmallVector<Value *, 4> destOperands;
    bool r = parseSuccessorAndUseList(dest, destOperands);
    destinations.push_back(dest);
    operands.push_back(destOperands);
    return r ? ParseFailure : ParseSuccess;
  };
  return parseCommaSeparatedListUntil(Token::r_square, parseElt,
                                      /*allowEmptyList=*/false);
}

/// Parse a (possibly empty) list of SSA operands with types as block arguments.
///
///   ssa-id-and-type-list ::= ssa-id-and-type (`,` ssa-id-and-type)*
///
ParseResult FunctionParser::parseOptionalBlockArgList(
    SmallVectorImpl<BlockArgument *> &results, Block *owner) {
  if (getToken().is(Token::r_brace))
    return ParseSuccess;

  // If the block already has arguments, then we're handling the entry block.
  // Parse and register the names for the arguments, but do not add them.
  bool definingExistingArgs = owner->getNumArguments() != 0;
  unsigned nextArgument = 0;

  return parseCommaSeparatedList([&]() -> ParseResult {
    auto type = parseSSADefOrUseAndType<Type>(
        [&](SSAUseInfo useInfo, Type type) -> Type {
          BlockArgument *arg;
          if (!definingExistingArgs) {
            arg = owner->addArgument(type);
          } else if (nextArgument >= owner->getNumArguments()) {
            emitError("too many arguments specified in argument list");
            return {};
          } else {
            arg = owner->getArgument(nextArgument++);
            if (arg->getType() != type) {
              emitError("argument and block argument type mismatch");
              return {};
            }
          }

          if (addDefinition(useInfo, arg))
            return {};
          return type;
        });
    return type ? ParseSuccess : ParseFailure;
  });
}

/// Parse an operation.
///
///  operation ::=
///    (ssa-id `=`)? string '(' ssa-use-list? ')' attribute-dict?
///    `:` function-type trailing-location?
///
ParseResult FunctionParser::parseOperation() {
  auto loc = getToken().getLoc();

  StringRef resultID;
  if (getToken().is(Token::percent_identifier)) {
    resultID = getTokenSpelling();
    consumeToken(Token::percent_identifier);
    if (parseToken(Token::equal, "expected '=' after SSA name"))
      return ParseFailure;
  }

  Instruction *op;
  if (getToken().is(Token::bare_identifier) || getToken().isKeyword())
    op = parseCustomOperation();
  else if (getToken().is(Token::string))
    op = parseGenericOperation();
  else
    return emitError("expected operation name in quotes");

  // If parsing of the basic operation failed, then this whole thing fails.
  if (!op)
    return ParseFailure;

  // If the instruction had a name, register it.
  if (!resultID.empty()) {
    if (op->getNumResults() == 0)
      return emitError(loc, "cannot name an operation with no results");

    for (unsigned i = 0, e = op->getNumResults(); i != e; ++i)
      if (addDefinition({resultID, i, loc}, op->getResult(i)))
        return ParseFailure;
  }

  // Try to parse the optional trailing location.
  if (parseOptionalTrailingLocation(op))
    return ParseFailure;

  return ParseSuccess;
}

Instruction *FunctionParser::parseGenericOperation() {
  // Get location information for the operation.
  auto srcLocation = getEncodedSourceLocation(getToken().getLoc());

  auto name = getToken().getStringValue();
  if (name.empty())
    return (emitError("empty operation name is invalid"), nullptr);
  if (name.find('\0') != StringRef::npos)
    return (emitError("null character not allowed in operation name"), nullptr);

  consumeToken(Token::string);

  OperationState result(builder.getContext(), srcLocation, name);

  // Generic operations have a resizable operation list.
  result.setOperandListToResizable();

  // Parse the operand list.
  SmallVector<SSAUseInfo, 8> operandInfos;

  if (parseToken(Token::l_paren, "expected '(' to start operand list") ||
      parseOptionalSSAUseList(operandInfos) ||
      parseToken(Token::r_paren, "expected ')' to end operand list")) {
    return nullptr;
  }

  // Parse the successor list but don't add successors to the result yet to
  // avoid messing up with the argument order.
  SmallVector<Block *, 2> successors;
  SmallVector<SmallVector<Value *, 4>, 2> successorOperands;
  if (getToken().is(Token::l_square)) {
    // Check if the operation is a known terminator.
    const AbstractOperation *abstractOp = result.name.getAbstractOperation();
    if (abstractOp && !abstractOp->hasProperty(OperationProperty::Terminator))
      return emitError("successors in non-terminator"), nullptr;
    if (parseSuccessors(successors, successorOperands))
      return nullptr;
  }

  if (getToken().is(Token::l_brace)) {
    if (parseAttributeDict(result.attributes))
      return nullptr;
  }

  if (parseToken(Token::colon, "expected ':' followed by instruction type"))
    return nullptr;

  auto typeLoc = getToken().getLoc();
  auto type = parseType();
  if (!type)
    return nullptr;
  auto fnType = type.dyn_cast<FunctionType>();
  if (!fnType)
    return (emitError(typeLoc, "expected function type"), nullptr);

  result.addTypes(fnType.getResults());

  // Check that we have the right number of types for the operands.
  auto operandTypes = fnType.getInputs();
  if (operandTypes.size() != operandInfos.size()) {
    auto plural = "s"[operandInfos.size() == 1];
    return (emitError(typeLoc, "expected " + llvm::utostr(operandInfos.size()) +
                                   " operand type" + plural + " but had " +
                                   llvm::utostr(operandTypes.size())),
            nullptr);
  }

  // Resolve all of the operands.
  for (unsigned i = 0, e = operandInfos.size(); i != e; ++i) {
    result.operands.push_back(resolveSSAUse(operandInfos[i], operandTypes[i]));
    if (!result.operands.back())
      return nullptr;
  }

  // Add the sucessors, and their operands after the proper operands.
  for (const auto &succ : llvm::zip(successors, successorOperands)) {
    Block *successor = std::get<0>(succ);
    const SmallVector<Value *, 4> &operands = std::get<1>(succ);
    result.addSuccessor(successor, operands);
  }

  // Parse the optional regions for this operation.
  std::vector<SmallVector<Block *, 2>> blocks;
  while (getToken().is(Token::l_brace)) {
    SmallVector<Block *, 2> newBlocks;
    if (parseOperationRegion(newBlocks, /*entryArguments=*/llvm::None)) {
      for (auto &region : blocks)
        cleanupInvalidBlocks(region);
      return nullptr;
    }
    blocks.emplace_back(newBlocks);
  }
  result.reserveRegions(blocks.size());

  auto *opInst = builder.createOperation(result);

  // Initialize the parsed regions.
  for (unsigned i = 0, e = blocks.size(); i != e; ++i) {
    auto &region = opInst->getRegion(i).getBlocks();
    region.insert(region.end(), blocks[i].begin(), blocks[i].end());
  }
  return opInst;
}

namespace {
class CustomOpAsmParser : public OpAsmParser {
public:
  CustomOpAsmParser(SMLoc nameLoc, StringRef opName, FunctionParser &parser)
      : nameLoc(nameLoc), opName(opName), parser(parser) {}

  bool parseOperation(const AbstractOperation *opDefinition,
                      OperationState *opState) {
    if (opDefinition->parseAssembly(this, opState))
      return true;

    // Check that enough regions were reserved for those that were parsed.
    if (parsedRegions.size() > opState->numRegions) {
      return emitError(
          nameLoc,
          "parsed more regions than those reserved in the operation state");
    }

    // Check there were no dangling entry block arguments.
    if (!parsedRegionEntryArguments.empty()) {
      return emitError(
          nameLoc, "no region was attached to parsed entry block arguments");
    }

    // Check that none of the operands of the current operation reference an
    // entry block argument for any of the region.
    for (auto *entryArg : parsedRegionEntryArgumentPlaceholders)
      if (llvm::is_contained(opState->operands, entryArg))
        return emitError(nameLoc, "operand use before it's defined");

    return false;
  }

  //===--------------------------------------------------------------------===//
  // High level parsing methods.
  //===--------------------------------------------------------------------===//

  bool getCurrentLocation(llvm::SMLoc *loc) override {
    *loc = parser.getToken().getLoc();
    return false;
  }
  bool parseComma() override {
    return parser.parseToken(Token::comma, "expected ','");
  }
  bool parseEqual() override {
    return parser.parseToken(Token::equal, "expected '='");
  }

  bool parseType(Type &result) override {
    return !(result = parser.parseType());
  }

  bool parseColonType(Type &result) override {
    return parser.parseToken(Token::colon, "expected ':'") ||
           !(result = parser.parseType());
  }

  bool parseColonTypeList(SmallVectorImpl<Type> &result) override {
    if (parser.parseToken(Token::colon, "expected ':'"))
      return true;

    do {
      if (auto type = parser.parseType())
        result.push_back(type);
      else
        return true;

    } while (parser.consumeIf(Token::comma));
    return false;
  }

  bool parseTrailingOperandList(SmallVectorImpl<OperandType> &result,
                                int requiredOperandCount,
                                Delimiter delimiter) override {
    if (parser.getToken().is(Token::comma)) {
      parseComma();
      return parseOperandList(result, requiredOperandCount, delimiter);
    }
    if (requiredOperandCount != -1)
      return emitError(parser.getToken().getLoc(),
                       "expected " + Twine(requiredOperandCount) + " operands");
    return false;
  }

  /// Parse an optional keyword.
  bool parseOptionalKeyword(const char *keyword) override {
    // Check that the current token is a bare identifier or keyword.
    if (parser.getToken().isNot(Token::bare_identifier) &&
        !parser.getToken().isKeyword())
      return true;

    if (parser.getTokenSpelling() == keyword) {
      parser.consumeToken();
      return false;
    }
    return true;
  }

  /// Parse an arbitrary attribute of a given type and return it in result. This
  /// also adds the attribute to the specified attribute list with the specified
  /// name.
  bool parseAttribute(Attribute &result, Type type, StringRef attrName,
                      SmallVectorImpl<NamedAttribute> &attrs) override {
    result = parser.parseAttribute(type);
    if (!result)
      return true;

    attrs.push_back(parser.builder.getNamedAttr(attrName, result));
    return false;
  }

  /// Parse an arbitrary attribute and return it in result.  This also adds
  /// the attribute to the specified attribute list with the specified name.
  bool parseAttribute(Attribute &result, StringRef attrName,
                      SmallVectorImpl<NamedAttribute> &attrs) override {
    return parseAttribute(result, Type(), attrName, attrs);
  }

  /// If a named attribute list is present, parse is into result.
  bool
  parseOptionalAttributeDict(SmallVectorImpl<NamedAttribute> &result) override {
    if (parser.getToken().isNot(Token::l_brace))
      return false;
    return parser.parseAttributeDict(result) == ParseFailure;
  }

  /// Parse a function name like '@foo' and return the name in a form that can
  /// be passed to resolveFunctionName when a function type is available.
  virtual bool parseFunctionName(StringRef &result, llvm::SMLoc &loc) {
    loc = parser.getToken().getLoc();

    if (parser.getToken().isNot(Token::at_identifier))
      return emitError(loc, "expected function name");

    result = parser.getTokenSpelling();
    parser.consumeToken(Token::at_identifier);
    return false;
  }

  bool parseOperand(OperandType &result) override {
    FunctionParser::SSAUseInfo useInfo;
    if (parser.parseSSAUse(useInfo))
      return true;

    result = {useInfo.loc, useInfo.name, useInfo.number};
    return false;
  }

  bool parseSuccessorAndUseList(Block *&dest,
                                SmallVectorImpl<Value *> &operands) override {
    // Defer successor parsing to the function parsers.
    return parser.parseSuccessorAndUseList(dest, operands);
  }

  bool parseOperandList(SmallVectorImpl<OperandType> &result,
                        int requiredOperandCount = -1,
                        Delimiter delimiter = Delimiter::None) override {
    auto startLoc = parser.getToken().getLoc();

    // Handle delimiters.
    switch (delimiter) {
    case Delimiter::None:
      // Don't check for the absence of a delimiter if the number of operands
      // is unknown (and hence the operand list could be empty).
      if (requiredOperandCount == -1)
        break;
      // Token already matches an identifier and so can't be a delimiter.
      if (parser.getToken().is(Token::percent_identifier))
        break;
      // Test against known delimiters.
      if (parser.getToken().is(Token::l_paren) ||
          parser.getToken().is(Token::l_square))
        return emitError(startLoc, "unexpected delimiter");
      return emitError(startLoc, "invalid operand");
    case Delimiter::OptionalParen:
      if (parser.getToken().isNot(Token::l_paren))
        return false;
      LLVM_FALLTHROUGH;
    case Delimiter::Paren:
      if (parser.parseToken(Token::l_paren, "expected '(' in operand list"))
        return true;
      break;
    case Delimiter::OptionalSquare:
      if (parser.getToken().isNot(Token::l_square))
        return false;
      LLVM_FALLTHROUGH;
    case Delimiter::Square:
      if (parser.parseToken(Token::l_square, "expected '[' in operand list"))
        return true;
      break;
    }

    // Check for zero operands.
    if (parser.getToken().is(Token::percent_identifier)) {
      do {
        OperandType operand;
        if (parseOperand(operand))
          return true;
        result.push_back(operand);
      } while (parser.consumeIf(Token::comma));
    }

    // Handle delimiters.   If we reach here, the optional delimiters were
    // present, so we need to parse their closing one.
    switch (delimiter) {
    case Delimiter::None:
      break;
    case Delimiter::OptionalParen:
    case Delimiter::Paren:
      if (parser.parseToken(Token::r_paren, "expected ')' in operand list"))
        return true;
      break;
    case Delimiter::OptionalSquare:
    case Delimiter::Square:
      if (parser.parseToken(Token::r_square, "expected ']' in operand list"))
        return true;
      break;
    }

    if (requiredOperandCount != -1 && result.size() != requiredOperandCount)
      return emitError(startLoc,
                       "expected " + Twine(requiredOperandCount) + " operands");
    return false;
  }

  /// Resolve a parse function name and a type into a function reference.
  virtual bool resolveFunctionName(StringRef name, FunctionType type,
                                   llvm::SMLoc loc, Function *&result) {
    result = parser.resolveFunctionReference(name, loc, type);
    return result == nullptr;
  }

  /// Parses a region.
  bool parseRegion() override {
    SmallVector<Block *, 2> results;
    if (parser.parseOperationRegion(results, parsedRegionEntryArguments))
      return true;

    parsedRegionEntryArguments.clear();
    parsedRegions.emplace_back(results);
    return false;
  }

  /// Parses an argument for the entry block of the next region to be parsed.
  bool parseRegionEntryBlockArgument(Type argType) override {
    SmallVector<Value *, 1> argValues;
    OperandType operand;
    if (parseOperand(operand))
      return true;

    // Create a place holder for this argument.
    FunctionParser::SSAUseInfo operandInfo = {operand.name, operand.number,
                                              operand.location};
    if (auto *value = parser.resolveSSAUse(operandInfo, argType)) {
      parsedRegionEntryArguments.emplace_back(operandInfo, argType);
      // Track each of the placeholders so that we can detect invalid references
      // to region arguments.
      parsedRegionEntryArgumentPlaceholders.emplace_back(value);
      return false;
    }

    return true;
  }

  //===--------------------------------------------------------------------===//
  // Methods for interacting with the parser
  //===--------------------------------------------------------------------===//

  Builder &getBuilder() const override { return parser.builder; }

  llvm::SMLoc getNameLoc() const override { return nameLoc; }

  bool resolveOperand(const OperandType &operand, Type type,
                      SmallVectorImpl<Value *> &result) override {
    FunctionParser::SSAUseInfo operandInfo = {operand.name, operand.number,
                                              operand.location};
    if (auto *value = parser.resolveSSAUse(operandInfo, type)) {
      result.push_back(value);
      return false;
    }
    return true;
  }

  /// Emit a diagnostic at the specified location and return true.
  bool emitError(llvm::SMLoc loc, const Twine &message) override {
    // If we emit an error, then cleanup any parsed regions.
    for (auto &region : parsedRegions)
      parser.cleanupInvalidBlocks(region);
    parsedRegions.clear();

    parser.emitError(loc, "custom op '" + Twine(opName) + "' " + message);
    emittedError = true;
    return true;
  }

  bool didEmitError() const { return emittedError; }

  /// Returns the regions that were parsed.
  MutableArrayRef<SmallVector<Block *, 2>> getParsedRegions() {
    return parsedRegions;
  }

private:
  std::vector<SmallVector<Block *, 2>> parsedRegions;
  SmallVector<std::pair<FunctionParser::SSAUseInfo, Type>, 2>
      parsedRegionEntryArguments;
  SmallVector<Value *, 2> parsedRegionEntryArgumentPlaceholders;
  SMLoc nameLoc;
  StringRef opName;
  FunctionParser &parser;
  bool emittedError = false;
};
} // end anonymous namespace.

Instruction *FunctionParser::parseCustomOperation() {
  auto opLoc = getToken().getLoc();
  auto opName = getTokenSpelling();
  CustomOpAsmParser opAsmParser(opLoc, opName, *this);

  auto *opDefinition = AbstractOperation::lookup(opName, getContext());
  if (!opDefinition && !opName.contains('.')) {
    // If the operation name has no namespace prefix we treat it as a standard
    // operation and prefix it with "std".
    // TODO: Would it be better to just build a mapping of the registered
    // operations in the standard dialect?
    opDefinition =
        AbstractOperation::lookup(Twine("std." + opName).str(), getContext());
  }

  if (!opDefinition) {
    opAsmParser.emitError(opLoc, "is unknown");
    return nullptr;
  }

  consumeToken();

  // If the custom op parser crashes, produce some indication to help
  // debugging.
  std::string opNameStr = opName.str();
  llvm::PrettyStackTraceFormat fmt("MLIR Parser: custom op parser '%s'",
                                   opNameStr.c_str());

  // Get location information for the operation.
  auto srcLocation = getEncodedSourceLocation(opLoc);

  // Have the op implementation take a crack and parsing this.
  OperationState opState(builder.getContext(), srcLocation, opDefinition->name);
  if (opAsmParser.parseOperation(opDefinition, &opState))
    return nullptr;

  // If it emitted an error, we failed.
  if (opAsmParser.didEmitError())
    return nullptr;

  // Otherwise, we succeeded.  Use the state it parsed as our op information.
  auto *opInst = builder.createOperation(opState);

  // Resolve any parsed regions.
  auto parsedRegions = opAsmParser.getParsedRegions();
  for (unsigned i = 0, e = parsedRegions.size(); i != e; ++i) {
    auto &opRegion = opInst->getRegion(i).getBlocks();
    opRegion.insert(opRegion.end(), parsedRegions[i].begin(),
                    parsedRegions[i].end());
  }
  return opInst;
}

/// Parse an affine constraint.
///  affine-constraint ::= affine-expr `>=` `0`
///                      | affine-expr `==` `0`
///
/// isEq is set to true if the parsed constraint is an equality, false if it
/// is an inequality (greater than or equal).
///
AffineExpr AffineParser::parseAffineConstraint(bool *isEq) {
  AffineExpr expr = parseAffineExpr();
  if (!expr)
    return nullptr;

  if (consumeIf(Token::greater) && consumeIf(Token::equal) &&
      getToken().is(Token::integer)) {
    auto dim = getToken().getUnsignedIntegerValue();
    if (dim.hasValue() && dim.getValue() == 0) {
      consumeToken(Token::integer);
      *isEq = false;
      return expr;
    }
    return (emitError("expected '0' after '>='"), nullptr);
  }

  if (consumeIf(Token::equal) && consumeIf(Token::equal) &&
      getToken().is(Token::integer)) {
    auto dim = getToken().getUnsignedIntegerValue();
    if (dim.hasValue() && dim.getValue() == 0) {
      consumeToken(Token::integer);
      *isEq = true;
      return expr;
    }
    return (emitError("expected '0' after '=='"), nullptr);
  }

  return (emitError("expected '== 0' or '>= 0' at end of affine constraint"),
          nullptr);
}

/// Parse the constraints that are part of an integer set definition.
///  integer-set-inline
///                ::= dim-and-symbol-id-lists `:`
///                '(' affine-constraint-conjunction? ')'
///  affine-constraint-conjunction ::= affine-constraint (`,`
///                                       affine-constraint)*
///
IntegerSet AffineParser::parseIntegerSetConstraints(unsigned numDims,
                                                    unsigned numSymbols) {
  if (parseToken(Token::l_paren,
                 "expected '(' at start of integer set constraint list"))
    return IntegerSet();

  SmallVector<AffineExpr, 4> constraints;
  SmallVector<bool, 4> isEqs;
  auto parseElt = [&]() -> ParseResult {
    bool isEq;
    auto elt = parseAffineConstraint(&isEq);
    ParseResult res = elt ? ParseSuccess : ParseFailure;
    if (elt) {
      constraints.push_back(elt);
      isEqs.push_back(isEq);
    }
    return res;
  };

  // Parse a list of affine constraints (comma-separated).
  if (parseCommaSeparatedListUntil(Token::r_paren, parseElt, true))
    return IntegerSet();

  // If no constraints were parsed, then treat this as a degenerate 'true' case.
  if (constraints.empty()) {
    /* 0 == 0 */
    auto zero = getAffineConstantExpr(0, getContext());
    return builder.getIntegerSet(numDims, numSymbols, zero, true);
  }

  // Parsed a valid integer set.
  return builder.getIntegerSet(numDims, numSymbols, constraints, isEqs);
}

//===----------------------------------------------------------------------===//
// Top-level entity parsing.
//===----------------------------------------------------------------------===//

namespace {
/// This parser handles entities that are only valid at the top level of the
/// file.
class ModuleParser : public Parser {
public:
  explicit ModuleParser(ParserState &state) : Parser(state) {}

  ParseResult parseModule();

private:
  ParseResult finalizeModule();

  ParseResult parseAffineStructureDef();

  ParseResult parseTypeAliasDef();

  // Functions.
  ParseResult
  parseArgumentList(SmallVectorImpl<Type> &argTypes,
                    SmallVectorImpl<StringRef> &argNames,
                    SmallVectorImpl<SmallVector<NamedAttribute, 2>> &argAttrs);
  ParseResult parseFunctionSignature(
      StringRef &name, FunctionType &type, SmallVectorImpl<StringRef> &argNames,
      SmallVectorImpl<SmallVector<NamedAttribute, 2>> &argAttrs);
  ParseResult parseFunc();
};
} // end anonymous namespace

/// Parses either an affine map declaration or an integer set declaration.
///
/// Affine map declaration.
///
///   affine-map-def ::= affine-map-id `=` affine-map-inline
///
/// Integer set declaration.
///
///  integer-set-decl ::= integer-set-id `=` integer-set-inline
///
ParseResult ModuleParser::parseAffineStructureDef() {
  assert(getToken().is(Token::hash_identifier));

  StringRef affineStructureId = getTokenSpelling().drop_front();

  // Check for redefinitions.
  if (getState().affineMapDefinitions.count(affineStructureId) > 0)
    return emitError("redefinition of affine map id '" + affineStructureId +
                     "'");
  if (getState().integerSetDefinitions.count(affineStructureId) > 0)
    return emitError("redefinition of integer set id '" + affineStructureId +
                     "'");

  consumeToken(Token::hash_identifier);

  // Parse the '='
  if (parseToken(Token::equal,
                 "expected '=' in affine map outlined definition"))
    return ParseFailure;

  AffineMap map;
  IntegerSet set;
  if (AffineParser(getState()).parseAffineMapOrIntegerSetInline(map, set))
    return ParseFailure;

  if (map) {
    getState().affineMapDefinitions[affineStructureId] = map;
    return ParseSuccess;
  }

  assert(set);
  getState().integerSetDefinitions[affineStructureId] = set;
  return ParseSuccess;
}

/// Parse a type alias declaration.
///
///   type-alias-def ::= '!' alias-name `=` 'type' type
///
ParseResult ModuleParser::parseTypeAliasDef() {
  assert(getToken().is(Token::exclamation_identifier));

  StringRef aliasName = getTokenSpelling().drop_front();

  // Check for redefinitions.
  if (getState().typeAliasDefinitions.count(aliasName) > 0)
    return emitError("redefinition of type alias id '" + aliasName + "'");

  consumeToken(Token::exclamation_identifier);

  // Parse the '=' and 'type'.
  if (parseToken(Token::equal, "expected '=' in type alias definition") ||
      parseToken(Token::kw_type, "expected 'type' in type alias definition"))
    return ParseFailure;

  // Parse the type.
  Type aliasedType = parseType();
  if (!aliasedType)
    return ParseFailure;

  // Register this alias with the parser state.
  getState().typeAliasDefinitions.try_emplace(aliasName, aliasedType);

  return ParseSuccess;
}

/// Parse a (possibly empty) list of Function arguments with types.
///
///   named-argument ::= ssa-id `:` type attribute-dict?
///   argument-list  ::= named-argument (`,` named-argument)* | /*empty*/
///   argument-list ::= type attribute-dict? (`,` type attribute-dict?)*
///                     | /*empty*/
///
ParseResult ModuleParser::parseArgumentList(
    SmallVectorImpl<Type> &argTypes, SmallVectorImpl<StringRef> &argNames,
    SmallVectorImpl<SmallVector<NamedAttribute, 2>> &argAttrs) {
  consumeToken(Token::l_paren);

  // The argument list either has to consistently have ssa-id's followed by
  // types, or just be a type list.  It isn't ok to sometimes have SSA ID's and
  // sometimes not.
  auto parseElt = [&]() -> ParseResult {
    // Parse argument name if present.
    auto loc = getToken().getLoc();
    StringRef name = getTokenSpelling();
    if (consumeIf(Token::percent_identifier)) {
      // Reject this if the preceding argument was missing a name.
      if (argNames.empty() && !argTypes.empty())
        return emitError(loc, "expected type instead of SSA identifier");

      argNames.push_back(name);

      if (parseToken(Token::colon, "expected ':'"))
        return ParseFailure;
    } else {
      // Reject this if the preceding argument had a name.
      if (!argNames.empty())
        return emitError("expected SSA identifier");
    }

    // Parse argument type
    auto elt = parseType();
    if (!elt)
      return ParseFailure;
    argTypes.push_back(elt);

    // Parse the attribute dict.
    SmallVector<NamedAttribute, 2> attrs;
    if (getToken().is(Token::l_brace)) {
      if (parseAttributeDict(attrs))
        return ParseFailure;
    }
    argAttrs.push_back(attrs);
    return ParseSuccess;
  };

  return parseCommaSeparatedListUntil(Token::r_paren, parseElt);
}

/// Parse a function signature, starting with a name and including the
/// parameter list.
///
///   function-signature ::=
///      function-id `(` argument-list `)` (`->` type-list)?
///
ParseResult ModuleParser::parseFunctionSignature(
    StringRef &name, FunctionType &type, SmallVectorImpl<StringRef> &argNames,
    SmallVectorImpl<SmallVector<NamedAttribute, 2>> &argAttrs) {
  if (getToken().isNot(Token::at_identifier))
    return emitError("expected a function identifier like '@foo'");

  name = getTokenSpelling().drop_front();
  consumeToken(Token::at_identifier);

  if (getToken().isNot(Token::l_paren))
    return emitError("expected '(' in function signature");

  SmallVector<Type, 4> argTypes;
  if (parseArgumentList(argTypes, argNames, argAttrs))
    return ParseFailure;

  // Parse the return type if present.
  SmallVector<Type, 4> results;
  if (consumeIf(Token::arrow)) {
    if (parseFunctionResultTypes(results))
      return ParseFailure;
  }
  type = builder.getFunctionType(argTypes, results);
  return ParseSuccess;
}

/// Function declarations.
///
///   function ::= `func` function-signature function-attributes?
///                                          trailing-location? function-body?
///   function-body ::= `{` block+ `}`
///   function-attributes ::= `attributes` attribute-dict
///
ParseResult ModuleParser::parseFunc() {
  consumeToken();

  StringRef name;
  FunctionType type;
  SmallVector<StringRef, 4> argNames;
  SmallVector<SmallVector<NamedAttribute, 2>, 4> argAttrs;

  auto loc = getToken().getLoc();
  if (parseFunctionSignature(name, type, argNames, argAttrs))
    return ParseFailure;

  // If function attributes are present, parse them.
  SmallVector<NamedAttribute, 8> attrs;
  if (consumeIf(Token::kw_attributes)) {
    if (parseAttributeDict(attrs))
      return ParseFailure;
  }

  // Okay, the function signature was parsed correctly, create the function now.
  auto *function =
      new Function(getEncodedSourceLocation(loc), name, type, attrs);
  getModule()->getFunctions().push_back(function);

  // Verify no name collision / redefinition.
  if (function->getName() != name)
    return emitError(loc,
                     "redefinition of function named '" + name.str() + "'");

  // Parse an optional trailing location.
  if (parseOptionalTrailingLocation(function))
    return ParseFailure;

  // Add the attributes to the function arguments.
  for (unsigned i = 0, e = function->getNumArguments(); i != e; ++i)
    function->setArgAttrs(i, argAttrs[i]);

  // External functions have no body.
  if (getToken().isNot(Token::l_brace))
    return ParseSuccess;

  // Create the parser.
  auto parser = FunctionParser(getState(), function);

  bool hadNamedArguments = !argNames.empty();

  // Add the entry block and argument list.
  function->addEntryBlock();

  // Add definitions of the function arguments.
  if (hadNamedArguments) {
    for (unsigned i = 0, e = function->getNumArguments(); i != e; ++i) {
      if (parser.addDefinition({argNames[i], 0, loc}, function->getArgument(i)))
        return ParseFailure;
    }
  }

  return parser.parseFunctionBody(hadNamedArguments);
}

/// Finish the end of module parsing - when the result is valid, do final
/// checking.
ParseResult ModuleParser::finalizeModule() {

  // Resolve all forward references, building a remapping table of attributes.
  DenseMap<Attribute, FunctionAttr> remappingTable;
  for (auto forwardRef : getState().functionForwardRefs) {
    auto name = forwardRef.first;

    // Resolve the reference.
    auto *resolvedFunction = getModule()->getNamedFunction(name);
    if (!resolvedFunction) {
      forwardRef.second->emitError("reference to undefined function '" +
                                   name.str() + "'");
      return ParseFailure;
    }

    remappingTable[builder.getFunctionAttr(forwardRef.second)] =
        builder.getFunctionAttr(resolvedFunction);
  }

  // If there was nothing to remap, then we're done.
  if (remappingTable.empty())
    return ParseSuccess;

  // Otherwise, walk the entire module replacing uses of one attribute set
  // with the correct ones.
  remapFunctionAttrs(*getModule(), remappingTable);

  // Now that all references to the forward definition placeholders are
  // resolved, we can deallocate the placeholders.
  for (auto forwardRef : getState().functionForwardRefs)
    delete forwardRef.second;
  getState().functionForwardRefs.clear();
  return ParseSuccess;
}

/// This is the top-level module parser.
ParseResult ModuleParser::parseModule() {
  while (1) {
    switch (getToken().getKind()) {
    default:
      emitError("expected a top level entity");
      return ParseFailure;

      // If we got to the end of the file, then we're done.
    case Token::eof:
      return finalizeModule();

    // If we got an error token, then the lexer already emitted an error, just
    // stop.  Someday we could introduce error recovery if there was demand
    // for it.
    case Token::error:
      return ParseFailure;

    case Token::hash_identifier:
      if (parseAffineStructureDef())
        return ParseFailure;
      break;

    case Token::exclamation_identifier:
      if (parseTypeAliasDef())
        return ParseFailure;
      break;

    case Token::kw_func:
      if (parseFunc())
        return ParseFailure;
      break;
    }
  }
}

//===----------------------------------------------------------------------===//

/// This parses the file specified by the indicated SourceMgr and returns an
/// MLIR module if it was valid.  If not, it emits diagnostics and returns
/// null.
Module *mlir::parseSourceFile(const llvm::SourceMgr &sourceMgr,
                              MLIRContext *context) {

  // This is the result module we are parsing into.
  std::unique_ptr<Module> module(new Module(context));

  ParserState state(sourceMgr, module.get());
  if (ModuleParser(state).parseModule()) {
    return nullptr;
  }

  // Make sure the parse module has no other structural problems detected by
  // the verifier.
  if (module->verify())
    return nullptr;

  return module.release();
}

/// This parses the file specified by the indicated filename and returns an
/// MLIR module if it was valid.  If not, the error message is emitted through
/// the error handler registered in the context, and a null pointer is returned.
Module *mlir::parseSourceFile(StringRef filename, MLIRContext *context) {
  auto file_or_err = llvm::MemoryBuffer::getFile(filename);
  if (std::error_code error = file_or_err.getError()) {
    context->emitError(mlir::UnknownLoc::get(context),
                       "Could not open input file " + filename);
    return nullptr;
  }

  // Load the MLIR module.
  llvm::SourceMgr source_mgr;
  source_mgr.AddNewSourceBuffer(std::move(*file_or_err), llvm::SMLoc());
  return parseSourceFile(source_mgr, context);
}

/// This parses the program string to a MLIR module if it was valid. If not,
/// it emits diagnostics and returns null.
Module *mlir::parseSourceString(StringRef moduleStr, MLIRContext *context) {
  auto memBuffer = MemoryBuffer::getMemBuffer(moduleStr);
  if (!memBuffer)
    return nullptr;

  SourceMgr sourceMgr;
  sourceMgr.AddNewSourceBuffer(std::move(memBuffer), SMLoc());
  return parseSourceFile(sourceMgr, context);
}
