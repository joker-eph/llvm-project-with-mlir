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
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include <algorithm>
using namespace mlir;
using llvm::MemoryBuffer;
using llvm::SMLoc;
using llvm::SourceMgr;

namespace {
class Parser;

//===----------------------------------------------------------------------===//
// ParserState
//===----------------------------------------------------------------------===//

/// This class refers to all of the state maintained globally by the parser,
/// such as the current lexer position etc. The Parser base class provides
/// methods to access this.
class ParserState {
public:
  ParserState(const llvm::SourceMgr &sourceMgr, Module *module)
      : context(module->getContext()), module(module), lex(sourceMgr, context),
        curToken(lex.lexToken()) {}

  // A map from attribute alias identifier to Attribute.
  llvm::StringMap<Attribute> attributeAliasDefinitions;

  // A map from type alias identifier to Type.
  llvm::StringMap<Type> typeAliasDefinitions;

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

//===----------------------------------------------------------------------===//
// Parser
//===----------------------------------------------------------------------===//

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

  /// Parse a comma-separated list of elements up until the specified end token.
  ParseResult
  parseCommaSeparatedListUntil(Token::Kind rightToken,
                               const std::function<ParseResult()> &parseElement,
                               bool allowEmptyList = true);

  /// Parse a comma separated list of elements that must have at least one entry
  /// in it.
  ParseResult
  parseCommaSeparatedList(const std::function<ParseResult()> &parseElement);

  ParseResult parsePrettyDialectSymbolName(StringRef &prettyName);

  // We have two forms of parsing methods - those that return a non-null
  // pointer on success, and those that return a ParseResult to indicate whether
  // they returned a failure.  The second class fills in by-reference arguments
  // as the results of their action.

  //===--------------------------------------------------------------------===//
  // Error Handling
  //===--------------------------------------------------------------------===//

  /// Emit an error and return failure.
  InFlightDiagnostic emitError(const Twine &message = {}) {
    return emitError(state.curToken.getLoc(), message);
  }
  InFlightDiagnostic emitError(SMLoc loc, const Twine &message = {});

  /// Encode the specified source location information into an attribute for
  /// attachment to the IR.
  Location getEncodedSourceLocation(llvm::SMLoc loc) {
    return state.lex.getEncodedSourceLocation(loc);
  }

  //===--------------------------------------------------------------------===//
  // Token Parsing
  //===--------------------------------------------------------------------===//

  /// Return the current token the parser is inspecting.
  const Token &getToken() const { return state.curToken; }
  StringRef getTokenSpelling() const { return state.curToken.getSpelling(); }

  /// If the current token has the specified kind, consume it and return true.
  /// If not, return false.
  bool consumeIf(Token::Kind kind) {
    if (state.curToken.isNot(kind))
      return false;
    consumeToken(kind);
    return true;
  }

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

  /// Consume the specified token if present and return success.  On failure,
  /// output a diagnostic and return failure.
  ParseResult parseToken(Token::Kind expectedToken, const Twine &message);

  //===--------------------------------------------------------------------===//
  // Type Parsing
  //===--------------------------------------------------------------------===//

  ParseResult parseFunctionResultTypes(SmallVectorImpl<Type> &elements);
  ParseResult parseTypeListNoParens(SmallVectorImpl<Type> &elements);
  ParseResult parseTypeListParens(SmallVectorImpl<Type> &elements);

  /// Parse an arbitrary type.
  Type parseType();

  /// Parse a complex type.
  Type parseComplexType();

  /// Parse an extended type.
  Type parseExtendedType();

  /// Parse a function type.
  Type parseFunctionType();

  /// Parse a memref type.
  Type parseMemRefType();

  /// Parse a non function type.
  Type parseNonFunctionType();

  /// Parse a tensor type.
  Type parseTensorType();

  /// Parse a tuple type.
  Type parseTupleType();

  /// Parse a vector type.
  VectorType parseVectorType();
  ParseResult parseDimensionListRanked(SmallVectorImpl<int64_t> &dimensions,
                                       bool allowDynamic = true);
  ParseResult parseXInDimensionList();

  //===--------------------------------------------------------------------===//
  // Attribute Parsing
  //===--------------------------------------------------------------------===//

  /// Parse an arbitrary attribute with an optional type.
  Attribute parseAttribute(Type type = {});

  /// Parse an attribute dictionary.
  ParseResult parseAttributeDict(SmallVectorImpl<NamedAttribute> &attributes);

  /// Parse an extended attribute.
  Attribute parseExtendedAttr(Type type);

  /// Parse a float attribute.
  Attribute parseFloatAttr(Type type, bool isNegative);

  /// Parse an integer attribute.
  Attribute parseIntegerAttr(Type type, bool isSigned);

  /// Parse an opaque elements attribute.
  Attribute parseOpaqueElementsAttr();

  /// Parse a sparse elements attribute.
  Attribute parseSparseElementsAttr();

  /// Parse a splat elements attribute.
  Attribute parseSplatElementsAttr();

  /// Parse a dense elements attribute.
  Attribute parseDenseElementsAttr();
  DenseElementsAttr parseDenseElementsAttrAsTensor(Type eltType);
  ShapedType parseElementsLiteralType();

  //===--------------------------------------------------------------------===//
  // Location Parsing
  //===--------------------------------------------------------------------===//

  /// Parse an inline location.
  ParseResult parseLocation(llvm::Optional<Location> *loc);

  /// Parse a raw location instance.
  ParseResult parseLocationInstance(llvm::Optional<Location> *loc);

  /// Parse an optional trailing location.
  ///
  ///   trailing-location     ::= location?
  ///
  template <typename Owner>
  ParseResult parseOptionalTrailingLocation(Owner *owner) {
    // If there is a 'loc' we parse a trailing location.
    if (!getToken().is(Token::kw_loc))
      return success();

    // Parse the location.
    llvm::Optional<Location> directLoc;
    if (parseLocation(&directLoc))
      return failure();
    owner->setLoc(*directLoc);
    return success();
  }

  //===--------------------------------------------------------------------===//
  // Affine Parsing
  //===--------------------------------------------------------------------===//

  ParseResult parseAffineMapOrIntegerSetReference(AffineMap &map,
                                                  IntegerSet &set);

private:
  /// The Parser is subclassed and reinstantiated.  Do not add additional
  /// non-trivial state here, add it to the ParserState class.
  ParserState &state;
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Helper methods.
//===----------------------------------------------------------------------===//

/// Parse a comma separated list of elements that must have at least one entry
/// in it.
ParseResult Parser::parseCommaSeparatedList(
    const std::function<ParseResult()> &parseElement) {
  // Non-empty case starts with an element.
  if (parseElement())
    return failure();

  // Otherwise we have a list of comma separated elements.
  while (consumeIf(Token::comma)) {
    if (parseElement())
      return failure();
  }
  return success();
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
    return success();
  }

  if (parseCommaSeparatedList(parseElement) ||
      parseToken(rightToken, "expected ',' or '" +
                                 Token::getTokenSpelling(rightToken) + "'"))
    return failure();

  return success();
}

/// Parse the body of a pretty dialect symbol, which starts and ends with <>'s,
/// and may be recursive.  Return with the 'prettyName' StringRef encompasing
/// the entire pretty name.
///
///   pretty-dialect-sym-body ::= '<' pretty-dialect-sym-contents+ '>'
///   pretty-dialect-sym-contents ::= pretty-dialect-sym-body
///                                  | '(' pretty-dialect-sym-contents+ ')'
///                                  | '[' pretty-dialect-sym-contents+ ']'
///                                  | '{' pretty-dialect-sym-contents+ '}'
///                                  | '[^[<({>\])}\0]+'
///
ParseResult Parser::parsePrettyDialectSymbolName(StringRef &prettyName) {
  // Pretty symbol names are a relatively unstructured format that contains a
  // series of properly nested punctuation, with anything else in the middle.
  // Scan ahead to find it and consume it if successful, otherwise emit an
  // error.
  auto *curPtr = getTokenSpelling().data();

  SmallVector<char, 8> nestedPunctuation;

  // Scan over the nested punctuation, bailing out on error and consuming until
  // we find the end.  We know that we're currently looking at the '<', so we
  // can go until we find the matching '>' character.
  assert(*curPtr == '<');
  do {
    char c = *curPtr++;
    switch (c) {
    case '\0':
      // This also handles the EOF case.
      return emitError("unexpected nul or EOF in pretty dialect name");
    case '<':
    case '[':
    case '(':
    case '{':
      nestedPunctuation.push_back(c);
      continue;

    case '>':
      if (nestedPunctuation.pop_back_val() != '<')
        return emitError("unbalanced '>' character in pretty dialect name");
      break;
    case ']':
      if (nestedPunctuation.pop_back_val() != '[')
        return emitError("unbalanced ']' character in pretty dialect name");
      break;
    case ')':
      if (nestedPunctuation.pop_back_val() != '(')
        return emitError("unbalanced ')' character in pretty dialect name");
      break;
    case '}':
      if (nestedPunctuation.pop_back_val() != '{')
        return emitError("unbalanced '}' character in pretty dialect name");
      break;

    default:
      continue;
    }
  } while (!nestedPunctuation.empty());

  // Ok, we succeeded, remember where we stopped, reset the lexer to know it is
  // consuming all this stuff, and return.
  state.lex.resetPointer(curPtr);

  unsigned length = curPtr - prettyName.begin();
  prettyName = StringRef(prettyName.begin(), length);
  consumeToken();
  return success();
}

/// Parse an extended dialect symbol.
template <typename Symbol, typename SymbolAliasMap, typename CreateFn>
static Symbol parseExtendedSymbol(Parser &p, Token::Kind identifierTok,
                                  SymbolAliasMap &aliases,
                                  CreateFn &&createSymbol) {
  // Parse the dialect namespace.
  StringRef identifier = p.getTokenSpelling().drop_front();
  auto loc = p.getToken().getLoc();
  p.consumeToken(identifierTok);

  // If there is no '<' token following this, and if the typename contains no
  // dot, then we are parsing a symbol alias.
  if (p.getToken().isNot(Token::less) && !identifier.contains('.')) {
    // Check for an alias for this type.
    auto aliasIt = aliases.find(identifier);
    if (aliasIt == aliases.end())
      return (p.emitError("undefined symbol alias id '" + identifier + "'"),
              nullptr);
    return aliasIt->second;
  }

  // Otherwise, we are parsing a dialect-specific symbol.  If the name contains
  // a dot, then this is the "pretty" form.  If not, it is the verbose form that
  // looks like <"...">.
  std::string symbolData;
  auto dialectName = identifier;

  // Handle the verbose form, where "identifier" is a simple dialect name.
  if (!identifier.contains('.')) {
    // Consume the '<'.
    if (p.parseToken(Token::less, "expected '<' in dialect type"))
      return nullptr;

    // Parse the symbol specific data.
    if (p.getToken().isNot(Token::string))
      return (p.emitError("expected string literal data in dialect symbol"),
              nullptr);
    symbolData = p.getToken().getStringValue();
    loc = p.getToken().getLoc();
    p.consumeToken(Token::string);

    // Consume the '>'.
    if (p.parseToken(Token::greater, "expected '>' in dialect symbol"))
      return nullptr;
  } else {
    // Ok, the dialect name is the part of the identifier before the dot, the
    // part after the dot is the dialect's symbol, or the start thereof.
    auto dotHalves = identifier.split('.');
    dialectName = dotHalves.first;
    auto prettyName = dotHalves.second;

    // If the dialect's symbol is followed immediately by a <, then lex the body
    // of it into prettyName.
    if (p.getToken().is(Token::less) &&
        prettyName.bytes_end() == p.getTokenSpelling().bytes_begin()) {
      if (p.parsePrettyDialectSymbolName(prettyName))
        return nullptr;
    }

    symbolData = prettyName.str();
  }

  // Call into the provided symbol construction function.
  auto encodedLoc = p.getEncodedSourceLocation(loc);
  return createSymbol(dialectName, symbolData, encodedLoc);
}

//===----------------------------------------------------------------------===//
// Error Handling
//===----------------------------------------------------------------------===//

InFlightDiagnostic Parser::emitError(SMLoc loc, const Twine &message) {
  auto diag = getContext()->emitError(getEncodedSourceLocation(loc), message);

  // If we hit a parse error in response to a lexer error, then the lexer
  // already reported the error.
  if (getToken().is(Token::error))
    diag.abandon();
  return diag;
}

//===----------------------------------------------------------------------===//
// Token Parsing
//===----------------------------------------------------------------------===//

/// Consume the specified token if present and return success.  On failure,
/// output a diagnostic and return failure.
ParseResult Parser::parseToken(Token::Kind expectedToken,
                               const Twine &message) {
  if (consumeIf(expectedToken))
    return success();
  return emitError(message);
}

//===----------------------------------------------------------------------===//
// Type Parsing
//===----------------------------------------------------------------------===//

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
    return failure();
  elements.push_back(t);
  return success();
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
    return elt ? success() : failure();
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
    return failure();

  // Handle empty lists.
  if (getToken().is(Token::r_paren))
    return consumeToken(), success();

  if (parseTypeListNoParens(elements) ||
      parseToken(Token::r_paren, "expected ')'"))
    return failure();
  return success();
}

/// Parse a complex type.
///
///   complex-type ::= `complex` `<` type `>`
///
Type Parser::parseComplexType() {
  consumeToken(Token::kw_complex);

  // Parse the '<'.
  if (parseToken(Token::less, "expected '<' in complex type"))
    return nullptr;

  auto typeLocation = getEncodedSourceLocation(getToken().getLoc());
  auto elementType = parseType();
  if (!elementType ||
      parseToken(Token::greater, "expected '>' in complex type"))
    return nullptr;

  return ComplexType::getChecked(elementType, typeLocation);
}

/// Parse an extended type.
///
///   extended-type ::= (dialect-type | type-alias)
///   dialect-type  ::= `!` dialect-namespace `<` `"` type-data `"` `>`
///   dialect-type  ::= `!` alias-name pretty-dialect-attribute-body?
///   type-alias    ::= `!` alias-name
///
Type Parser::parseExtendedType() {
  return parseExtendedSymbol<Type>(
      *this, Token::exclamation_identifier, state.typeAliasDefinitions,
      [&](StringRef dialectName, StringRef symbolData, Location loc) -> Type {
        // If we found a registered dialect, then ask it to parse the type.
        if (auto *dialect = state.context->getRegisteredDialect(dialectName))
          return dialect->parseType(symbolData, loc);

        // Otherwise, form a new opaque type.
        return OpaqueType::getChecked(
            Identifier::get(dialectName, state.context), symbolData,
            state.context, loc);
      });
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
      auto affineMap = parseAttribute();
      if (!affineMap)
        return failure();

      // Verify that the parsed attribute is an affine map.
      if (auto affineMapAttr = affineMap.dyn_cast<AffineMapAttr>())
        affineMapComposition.push_back(affineMapAttr.getValue());
      else
        return emitError("expected affine map in memref type");
    }
    return success();
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

/// Parse any type except the function type.
///
///   non-function-type ::= integer-type
///                       | index-type
///                       | float-type
///                       | extended-type
///                       | vector-type
///                       | tensor-type
///                       | memref-type
///                       | complex-type
///                       | tuple-type
///                       | none-type
///
///   index-type ::= `index`
///   float-type ::= `f16` | `bf16` | `f32` | `f64`
///   none-type ::= `none`
///
Type Parser::parseNonFunctionType() {
  switch (getToken().getKind()) {
  default:
    return (emitError("expected non-function type"), nullptr);
  case Token::kw_memref:
    return parseMemRefType();
  case Token::kw_tensor:
    return parseTensorType();
  case Token::kw_complex:
    return parseComplexType();
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

  // none-type
  case Token::kw_none:
    consumeToken(Token::kw_none);
    return builder.getNoneType();

  // extended type
  case Token::exclamation_identifier:
    return parseExtendedType();
  }
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
                                 bool allowDynamic) {
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
      return failure();
  }

  return success();
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

  return success();
}

//===----------------------------------------------------------------------===//
// Attribute parsing.
//===----------------------------------------------------------------------===//

/// Parse an arbitrary attribute.
///
///  attribute-value ::= `unit`
///                    | bool-literal
///                    | integer-literal (`:` (index-type | integer-type))?
///                    | float-literal (`:` float-type)?
///                    | string-literal
///                    | type
///                    | `[` (attribute-value (`,` attribute-value)*)? `]`
///                    | `{` (attribute-entry (`,` attribute-entry)*)? `}`
///                    | function-id `:` function-type
///                    | (`splat` | `dense`) `<` (tensor-type | vector-type) `,`
///                      attribute-value `>`
///                    | `sparse` `<` (tensor-type | vector-type)`,`
///                          attribute-value `,` attribute-value `>`
///                    | `opaque` `<` dialect-namespace  `,`
///                      (tensor-type | vector-type) `,` hex-string-literal `>`
///                    | extended-attribute
///
Attribute Parser::parseAttribute(Type type) {
  switch (getToken().getKind()) {
  // Parse an AffineMap or IntegerSet attribute.
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

  // Parse an array attribute.
  case Token::l_square: {
    consumeToken(Token::l_square);

    SmallVector<Attribute, 4> elements;
    auto parseElt = [&]() -> ParseResult {
      elements.push_back(parseAttribute());
      return elements.back() ? success() : failure();
    };

    if (parseCommaSeparatedListUntil(Token::r_square, parseElt))
      return nullptr;
    return builder.getArrayAttr(elements);
  }

  // Parse a boolean attribute.
  case Token::kw_false:
    consumeToken(Token::kw_false);
    return builder.getBoolAttr(false);
  case Token::kw_true:
    consumeToken(Token::kw_true);
    return builder.getBoolAttr(true);

  // Parse a dense elements attribute.
  case Token::kw_dense:
    return parseDenseElementsAttr();

  // Parse a dictionary attribute.
  case Token::l_brace: {
    SmallVector<NamedAttribute, 4> elements;
    if (parseAttributeDict(elements))
      return nullptr;
    return builder.getDictionaryAttr(elements);
  }

  // Parse an extended attribute, i.e. alias or dialect attribute.
  case Token::hash_identifier:
    return parseExtendedAttr(type);

  // Parse floating point and integer attributes.
  case Token::floatliteral:
    return parseFloatAttr(type, /*isNegative=*/false);
  case Token::integer:
    return parseIntegerAttr(type, /*isSigned=*/false);
  case Token::minus: {
    consumeToken(Token::minus);
    if (getToken().is(Token::integer))
      return parseIntegerAttr(type, /*isSigned=*/true);
    if (getToken().is(Token::floatliteral))
      return parseFloatAttr(type, /*isNegative=*/true);

    return (emitError("expected constant integer or floating point value"),
            nullptr);
  }

  // Parse a function attribute.
  case Token::at_identifier: {
    auto nameStr = getTokenSpelling();
    consumeToken(Token::at_identifier);
    return builder.getFunctionAttr(nameStr.drop_front());
  }

  // Parse an opaque elements attribute.
  case Token::kw_opaque:
    return parseOpaqueElementsAttr();

  // Parse a sparse elements attribute.
  case Token::kw_sparse:
    return parseSparseElementsAttr();

  // Parse a splat elements attribute.
  case Token::kw_splat:
    return parseSplatElementsAttr();

  // Parse a string attribute.
  case Token::string: {
    auto val = getToken().getStringValue();
    consumeToken(Token::string);
    return builder.getStringAttr(val);
  }

  // Parse a 'unit' attribute.
  case Token::kw_unit:
    consumeToken(Token::kw_unit);
    return builder.getUnitAttr();

  default:
    // Parse a type attribute.
    if (Type type = parseType())
      return builder.getTypeAttr(type);
    return nullptr;
  }
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
    return failure();

  auto parseElt = [&]() -> ParseResult {
    // We allow keywords as attribute names.
    if (getToken().isNot(Token::bare_identifier, Token::inttype) &&
        !getToken().isKeyword())
      return emitError("expected attribute name");
    Identifier nameId = builder.getIdentifier(getTokenSpelling());
    consumeToken();

    // Try to parse the ':' for the attribute value.
    if (!consumeIf(Token::colon)) {
      // If there is no ':', we treat this as a unit attribute.
      attributes.push_back({nameId, builder.getUnitAttr()});
      return success();
    }

    auto attr = parseAttribute();
    if (!attr)
      return failure();

    attributes.push_back({nameId, attr});
    return success();
  };

  if (parseCommaSeparatedListUntil(Token::r_brace, parseElt))
    return failure();

  return success();
}

/// Parse an extended attribute.
///
///   extended-attribute ::= (dialect-attribute | attribute-alias)
///   dialect-attribute  ::= `#` dialect-namespace `<` `"` attr-data `"` `>`
///   dialect-attribute  ::= `#` alias-name pretty-dialect-sym-body?
///   attribute-alias    ::= `#` alias-name
///
Attribute Parser::parseExtendedAttr(Type type) {
  Attribute attr = parseExtendedSymbol<Attribute>(
      *this, Token::hash_identifier, state.attributeAliasDefinitions,
      [&](StringRef dialectName, StringRef symbolData,
          Location loc) -> Attribute {
        // If we found a registered dialect, then ask it to parse the attribute.
        if (auto *dialect = state.context->getRegisteredDialect(dialectName))
          return dialect->parseAttribute(symbolData, loc);

        // Otherwise, form a new opaque attribute.
        return OpaqueAttr::getChecked(
            Identifier::get(dialectName, state.context), symbolData,
            state.context, loc);
      });

  // Ensure that the attribute has the same type as requested.
  if (type && attr.getType() != type) {
    emitError("attribute type different than expected: expected ")
        << type << ", but got " << attr.getType();
    return nullptr;
  }
  return attr;
}

/// Parse a float attribute.
Attribute Parser::parseFloatAttr(Type type, bool isNegative) {
  auto val = getToken().getFloatingPointValue();
  if (!val.hasValue())
    return (emitError("floating point value too large for attribute"), nullptr);
  auto valTok = getToken().getLoc();
  consumeToken(Token::floatliteral);
  if (!type) {
    // Default to F64 when no type is specified.
    if (!consumeIf(Token::colon))
      type = builder.getF64Type();
    else if (!(type = parseType()))
      return nullptr;
  }
  if (!type.isa<FloatType>())
    return (emitError("floating point value not valid for specified type"),
            nullptr);
  return FloatAttr::getChecked(type,
                               isNegative ? -val.getValue() : val.getValue(),
                               getEncodedSourceLocation(valTok));
}

/// Parse an integer attribute.
Attribute Parser::parseIntegerAttr(Type type, bool isSigned) {
  auto val = getToken().getUInt64IntegerValue();
  if (!val.hasValue() ||
      (isSigned ? (int64_t)-val.getValue() >= 0 : (int64_t)val.getValue() < 0))
    return (emitError("integer constant out of range for attribute"), nullptr);
  consumeToken(Token::integer);
  if (!type) {
    // Default to i64 if not type is specified.
    if (!consumeIf(Token::colon))
      type = builder.getIntegerType(64);
    else if (!(type = parseType()))
      return nullptr;
  }
  if (!type.isIntOrIndex())
    return (emitError("integer value not valid for specified type"), nullptr);

  int width = type.isIndex() ? 64 : type.getIntOrFloatBitWidth();
  APInt apInt(width, *val, isSigned);
  if (apInt != *val)
    return (emitError("integer constant out of range for attribute"), nullptr);
  return builder.getIntegerAttr(type, isSigned ? -apInt : apInt);
}

/// Parse an opaque elements attribute.
Attribute Parser::parseOpaqueElementsAttr() {
  consumeToken(Token::kw_opaque);
  if (parseToken(Token::less, "expected '<' after 'opaque'"))
    return nullptr;

  if (getToken().isNot(Token::string))
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

  auto type = parseElementsLiteralType();
  if (!type)
    return nullptr;

  if (parseToken(Token::comma, "expected ',' after elements literal type"))
    return nullptr;
  if (getToken().getKind() != Token::string)
    return (emitError("opaque string should start with '0x'"), nullptr);

  auto val = getToken().getStringValue();
  if (val.size() < 2 || val[0] != '0' || val[1] != 'x')
    return (emitError("opaque string should start with '0x'"), nullptr);

  val = val.substr(2);
  if (!llvm::all_of(val, llvm::isHexDigit))
    return (emitError("opaque string only contains hex digits"), nullptr);

  consumeToken(Token::string);
  if (parseToken(Token::greater, "expected '>'"))
    return nullptr;
  return builder.getOpaqueElementsAttr(dialect, type, llvm::fromHex(val));
}

/// Parse a sparse elements attribute.
Attribute Parser::parseSparseElementsAttr() {
  consumeToken(Token::kw_sparse);
  if (parseToken(Token::less, "Expected '<' after 'sparse'"))
    return nullptr;

  auto type = parseElementsLiteralType();
  if (!type)
    return nullptr;

  if (parseToken(Token::comma, "expected ',' after elements literal type"))
    return nullptr;
  if (getToken().isNot(Token::l_square))
    return emitError("expected '[' to start sparse tensor literal"), nullptr;

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
  auto valuesType = values.getType();
  if (valuesType.getRank() != 1)
    return (emitError("expected 1-d tensor for values"), nullptr);

  auto indicesType = indices.getType();
  auto sameShape = (indicesType.getRank() == 1) ||
                   (type.getRank() == indicesType.getDimSize(1));
  auto sameElementNum = indicesType.getDimSize(0) == valuesType.getDimSize(0);
  if (!sameShape || !sameElementNum) {
    emitError() << "expected shape ([" << type.getShape()
                << "]); inferred shape of indices literal (["
                << indicesType.getShape()
                << "]); inferred shape of values literal (["
                << valuesType.getShape() << "])";
    return nullptr;
  }

  if (parseToken(Token::greater, "expected '>'"))
    return nullptr;

  // Build the sparse elements attribute by the indices and values.
  return builder.getSparseElementsAttr(
      type, indices.cast<DenseIntElementsAttr>(), values);
}

/// Parse a splat elements attribute.
Attribute Parser::parseSplatElementsAttr() {
  consumeToken(Token::kw_splat);
  if (parseToken(Token::less, "expected '<' after 'splat'"))
    return nullptr;

  auto type = parseElementsLiteralType();
  if (!type)
    return nullptr;
  if (parseToken(Token::comma, "expected ',' after elements literal type"))
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
    return emitError("expected scalar constant inside tensor literal"), nullptr;
  }
}

namespace {
class TensorLiteralParser {
public:
  TensorLiteralParser(Parser &p, Type eltTy) : p(p), eltTy(eltTy) {}

  ParseResult parse() {
    if (p.getToken().is(Token::l_square))
      return parseList(shape);
    return parseElement();
  }

  ArrayRef<Attribute> getValues() const { return storage; }

  ArrayRef<int64_t> getShape() const { return shape; }

private:
  /// Parse a single element, returning failure if it isn't a valid element
  /// literal. For example:
  /// parseElement(1) -> Success, 1
  /// parseElement([1]) -> Failure
  ParseResult parseElement();

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

ParseResult TensorLiteralParser::parseElement() {
  switch (p.getToken().getKind()) {
  case Token::floatliteral:
  case Token::integer:
  case Token::minus: {
    auto result = p.parseAttribute(eltTy);
    if (!result)
      return failure();
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
    return p.emitError("expected element literal of primitive type");
  }
  return success();
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

  auto checkDims =
      [&](const llvm::SmallVectorImpl<int64_t> &prevDims,
          const llvm::SmallVectorImpl<int64_t> &newDims) -> ParseResult {
    if (prevDims == newDims)
      return success();
    return p.emitError("tensor literal is invalid; ranks are not consistent "
                       "between elements");
  };

  bool first = true;
  llvm::SmallVector<int64_t, 4> newDims;
  unsigned size = 0;
  auto parseCommaSeparatedList = [&]() -> ParseResult {
    llvm::SmallVector<int64_t, 4> thisDims;
    if (p.getToken().getKind() == Token::l_square) {
      if (parseList(thisDims))
        return failure();
    } else if (parseElement()) {
      return failure();
    }
    ++size;
    if (!first)
      return checkDims(newDims, thisDims);
    newDims = thisDims;
    first = false;
    return success();
  };
  if (p.parseCommaSeparatedListUntil(Token::r_square, parseCommaSeparatedList))
    return failure();

  // Return the sublists' dimensions with 'size' prepended.
  dims.clear();
  dims.push_back(size);
  dims.append(newDims.begin(), newDims.end());
  return success();
}

/// Parse a dense elements attribute.
///
///   dense-attr-list ::= `[` attribute-value `]`
///   attribute-value ::= integer-literal
///                     | float-literal
///                     | `[` (attribute-value (`,` attribute-value)*)? `]`
///
/// This method compares the shapes from the parsing result and that from the
/// input argument. It returns a constructed dense elements attribute if both
/// match.
Attribute Parser::parseDenseElementsAttr() {
  consumeToken(Token::kw_dense);
  if (parseToken(Token::less, "expected '<' after 'dense'"))
    return nullptr;

  auto type = parseElementsLiteralType();
  if (!type)
    return nullptr;

  if (parseToken(Token::comma, "expected ',' after elements literal type"))
    return nullptr;

  TensorLiteralParser literalParser(*this, type.getElementType());
  if (literalParser.parse())
    return nullptr;

  if (literalParser.getShape() != type.getShape()) {
    emitError() << "inferred shape of elements literal (["
                << literalParser.getShape() << "]) does not match type (["
                << type.getShape() << "])";
    return nullptr;
  }

  if (parseToken(Token::greater, "expected '>'"))
    return nullptr;

  return builder.getDenseElementsAttr(type, literalParser.getValues())
      .cast<DenseElementsAttr>();
}

/// Parse a dense elements attribute.
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

/// Shaped type for elements attribute.
///
///   elements-literal-type ::= vector-type | ranked-tensor-type
///
/// This method also checks the type has static shape.
ShapedType Parser::parseElementsLiteralType() {
  auto type = parseType();
  if (!type)
    return nullptr;

  if (!type.isa<RankedTensorType>() && !type.isa<VectorType>()) {
    emitError("elements literal must be a ranked tensor or vector type");
    return nullptr;
  }

  auto sType = type.cast<ShapedType>();
  if (!sType.hasStaticShape())
    return (emitError("elements literal type must have static shape"), nullptr);

  return sType;
}

//===----------------------------------------------------------------------===//
// Location parsing.
//===----------------------------------------------------------------------===//

/// Parse a location.
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
    return failure();
  return success();
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
        return failure();

      // Parse the column number.
      if (getToken().isNot(Token::integer))
        return emitError("expected integer column number in FileLineColLoc");
      auto column = getToken().getUnsignedIntegerValue();
      if (!column.hasValue())
        return emitError("expected integer column number in FileLineColLoc");
      consumeToken(Token::integer);

      auto file = UniquedFilename::get(str, ctx);
      *loc = FileLineColLoc::get(file, line.getValue(), column.getValue(), ctx);
      return success();
    }

    // Otherwise, this is a NameLoc.

    // Check for a child location.
    if (consumeIf(Token::l_paren)) {
      auto childSourceLoc = getToken().getLoc();

      // Parse the child location.
      llvm::Optional<Location> childLoc;
      if (parseLocationInstance(&childLoc))
        return failure();

      // The child must not be another NameLoc.
      if (childLoc->isa<NameLoc>())
        return emitError(childSourceLoc,
                         "child of NameLoc cannot be another NameLoc");
      *loc = NameLoc::get(Identifier::get(str, ctx), *childLoc, ctx);

      // Parse the closing ')'.
      if (parseToken(Token::r_paren,
                     "expected ')' after child location of NameLoc"))
        return failure();
    } else {
      *loc = NameLoc::get(Identifier::get(str, ctx), ctx);
    }

    return success();
  }

  // Check for a 'unknown' for an unknown location.
  if (getToken().is(Token::bare_identifier) &&
      getToken().getSpelling() == "unknown") {
    consumeToken(Token::bare_identifier);
    *loc = UnknownLoc::get(ctx);
    return success();
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
        return failure();
    }

    // Parse the '['.
    if (parseToken(Token::l_square, "expected '[' in fused location"))
      return failure();

    // Parse the internal locations.
    llvm::SmallVector<Location, 4> locations;
    do {
      llvm::Optional<Location> newLoc;
      if (parseLocationInstance(&newLoc))
        return failure();
      locations.push_back(*newLoc);

      // Parse the ','.
    } while (consumeIf(Token::comma));

    // Parse the ']'.
    if (parseToken(Token::r_square, "expected ']' in fused location"))
      return failure();

    // Return the fused location.
    if (metadata)
      *loc = FusedLoc::get(locations, metadata, getContext());
    else
      *loc = FusedLoc::get(locations, ctx);
    return success();
  }

  // Check for the 'callsite' signifying a callsite location.
  if (getToken().is(Token::bare_identifier) &&
      getToken().getSpelling() == "callsite") {
    consumeToken(Token::bare_identifier);

    // Parse the '('.
    if (parseToken(Token::l_paren, "expected '(' in callsite location"))
      return failure();

    // Parse the callee location.
    llvm::Optional<Location> calleeLoc;
    if (parseLocationInstance(&calleeLoc))
      return failure();

    // Parse the 'at'.
    if (getToken().isNot(Token::bare_identifier) ||
        getToken().getSpelling() != "at")
      return emitError("expected 'at' in callsite location");
    consumeToken(Token::bare_identifier);

    // Parse the caller location.
    llvm::Optional<Location> callerLoc;
    if (parseLocationInstance(&callerLoc))
      return failure();

    // Parse the ')'.
    if (parseToken(Token::r_paren, "expected ')' in callsite location"))
      return failure();

    // Return the callsite location.
    *loc = CallSiteLoc::get(*calleeLoc, *callerLoc, ctx);
    return success();
  }

  return emitError("expected location instance");
}

//===----------------------------------------------------------------------===//
// Affine parsing.
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

  AffineMap parseAffineMapRange(unsigned numDims, unsigned numSymbols);
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
  llvm_unreachable("Unknown AffineHighPrecOp");
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
  llvm_unreachable("Unknown AffineLowPrecOp");
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
      return emitError("redefinition of identifier '" + name + "'");
  }
  consumeToken(Token::bare_identifier);

  dimsAndSymbols.push_back({name, idExpr});
  return success();
}

/// Parse the list of dimensional identifiers to an affine map.
ParseResult AffineParser::parseDimIdList(unsigned &numDims) {
  if (parseToken(Token::l_paren,
                 "expected '(' at start of dimensional identifiers list")) {
    return failure();
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
    return failure();
  }
  if (!getToken().is(Token::l_square)) {
    numSymbols = 0;
    return success();
  }
  return parseSymbolIdList(numSymbols);
}

/// Parses an ambiguous affine map or integer set definition inline.
ParseResult AffineParser::parseAffineMapOrIntegerSetInline(AffineMap &map,
                                                           IntegerSet &set) {
  unsigned numDims = 0, numSymbols = 0;

  // List of dimensional and optional symbol identifiers.
  if (parseDimAndOptionalSymbolIdList(numDims, numSymbols)) {
    return failure();
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
    return map ? success() : failure();
  } else if (parseToken(Token::colon, "expected ':' or '['")) {
    return failure();
  }

  if ((set = parseIntegerSetConstraints(numDims, numSymbols)))
    return success();

  return failure();
}

/// Parse the range and sizes affine map definition inline.
///
///  affine-map ::= dim-and-symbol-id-lists `->` multi-dim-affine-expr
///
///  multi-dim-affine-expr ::= `(` affine-expr (`,` affine-expr)* `)
AffineMap AffineParser::parseAffineMapRange(unsigned numDims,
                                            unsigned numSymbols) {
  parseToken(Token::l_paren, "expected '(' at start of affine map range");

  SmallVector<AffineExpr, 4> exprs;
  auto parseElt = [&]() -> ParseResult {
    auto elt = parseAffineExpr();
    ParseResult res = elt ? success() : failure();
    exprs.push_back(elt);
    return res;
  };

  // Parse a multi-dimensional affine expression (a comma-separated list of
  // 1-d affine expressions); the list cannot be empty. Grammar:
  // multi-dim-affine-expr ::= `(` affine-expr (`,` affine-expr)* `)
  if (parseCommaSeparatedListUntil(Token::r_paren, parseElt, false))
    return AffineMap();

  // Parsed a valid affine map.
  return builder.getAffineMap(numDims, numSymbols, exprs);
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
    ParseResult res = elt ? success() : failure();
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

/// Parse an ambiguous reference to either and affine map or an integer set.
ParseResult Parser::parseAffineMapOrIntegerSetReference(AffineMap &map,
                                                        IntegerSet &set) {
  return AffineParser(state).parseAffineMapOrIntegerSetInline(map, set);
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
  OpBuilder builder;
#pragma clang diagnostic pop

  FunctionParser(ParserState &state, Function *function)
      : Parser(state), builder(function->getBody()), function(function) {}

  ~FunctionParser();

  /// After parsing is finished, this function must be called to see if there
  /// are any remaining issues.
  ParseResult finalize(SMLoc loc);

  //===--------------------------------------------------------------------===//
  // SSA Value Handling
  //===--------------------------------------------------------------------===//

  /// This represents a use of an SSA value in the program.  The first two
  /// entries in the tuple are the name and result number of a reference.  The
  /// third is the location of the reference, which is used in case this ends
  /// up being a use of an undefined value.
  struct SSAUseInfo {
    StringRef name;  // Value name, e.g. %42 or %abc
    unsigned number; // Number, specified with #12
    SMLoc loc;       // Location of first definition or use.
  };

  /// Push a new SSA name scope to the parser.
  void pushSSANameScope();

  /// Pop the last SSA name scope from the parser.
  ParseResult popSSANameScope();

  /// Register a definition of a value with the symbol table.
  ParseResult addDefinition(SSAUseInfo useInfo, Value *value);

  /// Parse an optional list of SSA uses into 'results'.
  ParseResult parseOptionalSSAUseList(SmallVectorImpl<SSAUseInfo> &results);

  /// Parse a single SSA use into 'result'.
  ParseResult parseSSAUse(SSAUseInfo &result);

  /// Given a reference to an SSA value and its type, return a reference. This
  /// returns null on failure.
  Value *resolveSSAUse(SSAUseInfo useInfo, Type type);

  ParseResult parseSSADefOrUseAndType(
      const std::function<ParseResult(SSAUseInfo, Type)> &action);

  ParseResult parseOptionalSSAUseAndTypeList(SmallVectorImpl<Value *> &results);

  /// Return the location of the value identified by its name and number if it
  /// has been already defined.  Placeholder values are considered undefined.
  llvm::Optional<SMLoc> getDefinitionLoc(StringRef name, unsigned number) {
    if (!values.count(name) || number >= values[name].size())
      return {};
    Value *value = values[name][number].first;
    if (value && !isForwardRefPlaceholder(value))
      return values[name][number].second;
    return {};
  }

  //===--------------------------------------------------------------------===//
  // Operation Parsing
  //===--------------------------------------------------------------------===//

  /// Parse an operation instance.
  ParseResult parseOperation();

  /// Parse a single operation successor and it's operand list.
  ParseResult parseSuccessorAndUseList(Block *&dest,
                                       SmallVectorImpl<Value *> &operands);

  /// Parse a comma-separated list of operation successors in brackets.
  ParseResult
  parseSuccessors(SmallVectorImpl<Block *> &destinations,
                  SmallVectorImpl<SmallVector<Value *, 4>> &operands);

  /// Parse an operation instance that is in the generic form.
  Operation *parseGenericOperation();

  /// Parse an operation instance that is in the op-defined custom form.
  Operation *parseCustomOperation();

  //===--------------------------------------------------------------------===//
  // Region Parsing
  //===--------------------------------------------------------------------===//

  /// Parse a region into 'region' with the provided entry block arguments.
  ParseResult parseRegion(Region &region,
                          ArrayRef<std::pair<SSAUseInfo, Type>> entryArguments);

  /// Parse a region body into 'region'.
  ParseResult parseRegionBody(Region &region);

  //===--------------------------------------------------------------------===//
  // Block Parsing
  //===--------------------------------------------------------------------===//

  /// Parse a new block into 'block'.
  ParseResult parseBlock(Block *&block);

  /// Parse a list of operations into 'block'.
  ParseResult parseBlockBody(Block *block);

  /// Parse a (possibly empty) list of block arguments.
  ParseResult
  parseOptionalBlockArgList(SmallVectorImpl<BlockArgument *> &results,
                            Block *owner);

  /// Get the block with the specified name, creating it if it doesn't
  /// already exist.  The location specified is the point of use, which allows
  /// us to diagnose references to blocks that are not defined precisely.
  Block *getBlockNamed(StringRef name, SMLoc loc);

  /// Define the block with the specified name. Returns the Block* or nullptr in
  /// the case of redefinition.
  Block *defineBlockNamed(StringRef name, SMLoc loc, Block *existing);

private:
  Function *function;

  /// Returns the info for a block at the current scope for the given name.
  std::pair<Block *, SMLoc> &getBlockInfoByName(StringRef name) {
    return blocksByName.back()[name];
  }

  /// Insert a new forward reference to the given block.
  void insertForwardRef(Block *block, SMLoc loc) {
    forwardRef.back().try_emplace(block, loc);
  }

  /// Erase any forward reference to the given block.
  bool eraseForwardRef(Block *block) { return forwardRef.back().erase(block); }

  /// Record that a definition was added at the current scope.
  void recordDefinition(StringRef def) {
    definitionsPerScope.back().insert(def);
  }

  /// Create a forward reference placeholder value with the given location and
  /// result type.
  Value *createForwardRefPlaceholder(SMLoc loc, Type type);

  /// Return true if this is a forward reference.
  bool isForwardRefPlaceholder(Value *value) {
    return forwardRefPlaceholders.count(value);
  }

  /// This keeps track of the block names as well as the location of the first
  /// reference for each nested name scope. This is used to diagnose invalid
  /// block references and memoize them.
  SmallVector<DenseMap<StringRef, std::pair<Block *, SMLoc>>, 2> blocksByName;
  SmallVector<DenseMap<Block *, SMLoc>, 2> forwardRef;

  /// This keeps track of all of the SSA values we are tracking for each name
  /// scope, indexed by their name. This has one entry per result number.
  llvm::StringMap<SmallVector<std::pair<Value *, SMLoc>, 1>> values;

  /// This keeps track of all of the values defined by a specific name scope.
  SmallVector<llvm::StringSet<>, 2> definitionsPerScope;

  /// These are all of the placeholders we've made along with the location of
  /// their first reference, to allow checking for use of undefined values.
  DenseMap<Value *, SMLoc> forwardRefPlaceholders;
};
} // end anonymous namespace

FunctionParser::~FunctionParser() {
  for (auto &fwd : forwardRefPlaceholders) {
    // Drop all uses of undefined forward declared reference and destroy
    // defining operation.
    fwd.first->dropAllUses();
    fwd.first->getDefiningOp()->destroy();
  }
}

/// After the function is finished parsing, this function checks to see if
/// there are any remaining issues.
ParseResult FunctionParser::finalize(SMLoc loc) {
  // Check for any forward references that are left.  If we find any, error
  // out.
  if (!forwardRefPlaceholders.empty()) {
    SmallVector<std::pair<const char *, Value *>, 4> errors;
    // Iteration over the map isn't deterministic, so sort by source location.
    for (auto entry : forwardRefPlaceholders)
      errors.push_back({entry.second.getPointer(), entry.first});
    llvm::array_pod_sort(errors.begin(), errors.end());

    for (auto entry : errors) {
      auto loc = SMLoc::getFromPointer(entry.first);
      emitError(loc, "use of undeclared SSA value name");
    }
    return failure();
  }

  return success();
}

//===----------------------------------------------------------------------===//
// SSA Value Handling
//===----------------------------------------------------------------------===//

void FunctionParser::pushSSANameScope() {
  blocksByName.push_back(DenseMap<StringRef, std::pair<Block *, SMLoc>>());
  forwardRef.push_back(DenseMap<Block *, SMLoc>());
  definitionsPerScope.push_back({});
}

ParseResult FunctionParser::popSSANameScope() {
  auto forwardRefInCurrentScope = forwardRef.pop_back_val();

  // Verify that all referenced blocks were defined.
  if (!forwardRefInCurrentScope.empty()) {
    SmallVector<std::pair<const char *, Block *>, 4> errors;
    // Iteration over the map isn't deterministic, so sort by source location.
    for (auto entry : forwardRefInCurrentScope) {
      errors.push_back({entry.second.getPointer(), entry.first});
      // Add this block to the top-level region to allow for automatic cleanup.
      function->push_back(entry.first);
    }
    llvm::array_pod_sort(errors.begin(), errors.end());

    for (auto entry : errors) {
      auto loc = SMLoc::getFromPointer(entry.first);
      emitError(loc, "reference to an undefined block");
    }
    return failure();
  }

  // Drop any values defined in this scope from the value map.
  for (auto &def : definitionsPerScope.pop_back_val())
    values.erase(def.getKey());
  blocksByName.pop_back();

  return success();
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
    if (!isForwardRefPlaceholder(existing)) {
      emitError(useInfo.loc)
          .append("redefinition of SSA value '", useInfo.name, "'")
          .attachNote(getEncodedSourceLocation(entries[useInfo.number].second))
          .append("previously defined here");
      return failure();
    }

    // If it was a forward reference, update everything that used it to use
    // the actual definition instead, delete the forward ref, and remove it
    // from our set of forward references we track.
    existing->replaceAllUsesWith(value);
    existing->getDefiningOp()->destroy();
    forwardRefPlaceholders.erase(existing);
  }

  /// Record this definition for the current scope.
  entries[useInfo.number] = {value, useInfo.loc};
  recordDefinition(useInfo.name);
  return success();
}

/// Parse a (possibly empty) list of SSA operands.
///
///   ssa-use-list ::= ssa-use (`,` ssa-use)*
///   ssa-use-list-opt ::= ssa-use-list?
///
ParseResult
FunctionParser::parseOptionalSSAUseList(SmallVectorImpl<SSAUseInfo> &results) {
  if (getToken().isNot(Token::percent_identifier))
    return success();
  return parseCommaSeparatedList([&]() -> ParseResult {
    SSAUseInfo result;
    if (parseSSAUse(result))
      return failure();
    results.push_back(result);
    return success();
  });
}

/// Parse a SSA operand for an operation.
///
///   ssa-use ::= ssa-id
///
ParseResult FunctionParser::parseSSAUse(SSAUseInfo &result) {
  result.name = getTokenSpelling();
  result.number = 0;
  result.loc = getToken().getLoc();
  if (parseToken(Token::percent_identifier, "expected SSA operand"))
    return failure();

  // If we have an attribute ID, it is a result number.
  if (getToken().is(Token::hash_identifier)) {
    if (auto value = getToken().getHashIdentifierNumber())
      result.number = value.getValue();
    else
      return emitError("invalid SSA value result number");
    consumeToken(Token::hash_identifier);
  }

  return success();
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

    emitError(useInfo.loc, "use of value '")
        .append(useInfo.name, "' expects different type than prior uses")
        .attachNote(getEncodedSourceLocation(entries[useInfo.number].second))
        .append("prior use here");
    return nullptr;
  }

  // Make sure we have enough slots for this.
  if (entries.size() <= useInfo.number)
    entries.resize(useInfo.number + 1);

  // If the value has already been defined and this is an overly large result
  // number, diagnose that.
  if (entries[0].first && !isForwardRefPlaceholder(entries[0].first))
    return (emitError(useInfo.loc, "reference to invalid result number"),
            nullptr);

  // Otherwise, this is a forward reference.  Create a placeholder and remember
  // that we did so.
  auto *result = createForwardRefPlaceholder(useInfo.loc, type);
  entries[useInfo.number].first = result;
  entries[useInfo.number].second = useInfo.loc;
  return result;
}

/// Parse an SSA use with an associated type.
///
///   ssa-use-and-type ::= ssa-use `:` type
ParseResult FunctionParser::parseSSADefOrUseAndType(
    const std::function<ParseResult(SSAUseInfo, Type)> &action) {
  SSAUseInfo useInfo;
  if (parseSSAUse(useInfo) ||
      parseToken(Token::colon, "expected ':' and type for SSA operand"))
    return failure();

  auto type = parseType();
  if (!type)
    return failure();

  return action(useInfo, type);
}

/// Parse a (possibly empty) list of SSA operands, followed by a colon, then
/// followed by a type list.
///
///   ssa-use-and-type-list
///     ::= ssa-use-list ':' type-list-no-parens
///
ParseResult FunctionParser::parseOptionalSSAUseAndTypeList(
    SmallVectorImpl<Value *> &results) {
  SmallVector<SSAUseInfo, 4> valueIDs;
  if (parseOptionalSSAUseList(valueIDs))
    return failure();

  // If there were no operands, then there is no colon or type lists.
  if (valueIDs.empty())
    return success();

  SmallVector<Type, 4> types;
  if (parseToken(Token::colon, "expected ':' in operand list") ||
      parseTypeListNoParens(types))
    return failure();

  if (valueIDs.size() != types.size())
    return emitError("expected ")
           << valueIDs.size() << " types to match operand list";

  results.reserve(valueIDs.size());
  for (unsigned i = 0, e = valueIDs.size(); i != e; ++i) {
    if (auto *value = resolveSSAUse(valueIDs[i], types[i]))
      results.push_back(value);
    else
      return failure();
  }

  return success();
}

/// Create and remember a new placeholder for a forward reference.
Value *FunctionParser::createForwardRefPlaceholder(SMLoc loc, Type type) {
  // Forward references are always created as operations, because we just need
  // something with a def/use chain.
  //
  // We create these placeholders as having an empty name, which we know
  // cannot be created through normal user input, allowing us to distinguish
  // them.
  auto name = OperationName("placeholder", getContext());
  auto *op = Operation::create(
      getEncodedSourceLocation(loc), name, /*operands=*/{}, type,
      /*attributes=*/llvm::None, /*successors=*/{}, /*numRegions=*/0,
      /*resizableOperandList=*/false, getContext());
  forwardRefPlaceholders[op->getResult(0)] = loc;
  return op->getResult(0);
}

//===----------------------------------------------------------------------===//
// Operation Parsing
//===----------------------------------------------------------------------===//

/// Parse an operation.
///
///  operation ::=
///    operation-result? string '(' ssa-use-list? ')' attribute-dict?
///    `:` function-type trailing-location?
///  operation-result ::= ssa-id ((`:` integer-literal) | (`,` ssa-id)*) `=`
///
ParseResult FunctionParser::parseOperation() {
  auto loc = getToken().getLoc();
  SmallVector<std::pair<StringRef, SMLoc>, 1> resultIDs;
  size_t numExpectedResults;
  if (getToken().is(Token::percent_identifier)) {
    // Parse the first result id.
    resultIDs.emplace_back(getTokenSpelling(), loc);
    consumeToken(Token::percent_identifier);

    // If the next token is a ':', we parse the expected result count.
    if (consumeIf(Token::colon)) {
      // Check that the next token is an integer.
      if (!getToken().is(Token::integer))
        return emitError("expected integer number of results");

      // Check that number of results is > 0.
      auto val = getToken().getUInt64IntegerValue();
      if (!val.hasValue() || val.getValue() < 1)
        return emitError("expected named operation to have atleast 1 result");
      consumeToken(Token::integer);
      numExpectedResults = *val;
    } else {
      // Otherwise, this is a comma separated list of result ids.
      if (consumeIf(Token::comma)) {
        auto parseNextResult = [&]() -> ParseResult {
          // Parse the next result id.
          if (!getToken().is(Token::percent_identifier))
            return emitError("expected valid ssa identifier");

          resultIDs.emplace_back(getTokenSpelling(), getToken().getLoc());
          consumeToken(Token::percent_identifier);
          return success();
        };

        if (parseCommaSeparatedList(parseNextResult))
          return failure();
      }
      numExpectedResults = resultIDs.size();
    }

    if (parseToken(Token::equal, "expected '=' after SSA name"))
      return failure();
  }

  Operation *op;
  if (getToken().is(Token::bare_identifier) || getToken().isKeyword())
    op = parseCustomOperation();
  else if (getToken().is(Token::string))
    op = parseGenericOperation();
  else
    return emitError("expected operation name in quotes");

  // If parsing of the basic operation failed, then this whole thing fails.
  if (!op)
    return failure();

  // If the operation had a name, register it.
  if (!resultIDs.empty()) {
    if (op->getNumResults() == 0)
      return emitError(loc, "cannot name an operation with no results");
    if (numExpectedResults != op->getNumResults())
      return emitError(loc, "operation defines ")
             << op->getNumResults() << " results but was provided "
             << numExpectedResults << " to bind";

    // If the number of result names matches the number of operation results, we
    // can directly use the provided names.
    if (resultIDs.size() == op->getNumResults()) {
      for (unsigned i = 0, e = op->getNumResults(); i != e; ++i)
        if (addDefinition({resultIDs[i].first, 0, resultIDs[i].second},
                          op->getResult(i)))
          return failure();
    } else {
      // Otherwise, we use the same name for all results.
      StringRef name = resultIDs.front().first;
      for (unsigned i = 0, e = op->getNumResults(); i != e; ++i)
        if (addDefinition({name, i, loc}, op->getResult(i)))
          return failure();
    }
  }

  // Try to parse the optional trailing location.
  if (parseOptionalTrailingLocation(op))
    return failure();

  return success();
}

/// Parse a single operation successor and it's operand list.
///
///   successor ::= block-id branch-use-list?
///   branch-use-list ::= `(` ssa-use-list ':' type-list-no-parens `)`
///
ParseResult
FunctionParser::parseSuccessorAndUseList(Block *&dest,
                                         SmallVectorImpl<Value *> &operands) {
  // Verify branch is identifier and get the matching block.
  if (!getToken().is(Token::caret_identifier))
    return emitError("expected block name");
  dest = getBlockNamed(getTokenSpelling(), getToken().getLoc());
  consumeToken();

  // Handle optional arguments.
  if (consumeIf(Token::l_paren) &&
      (parseOptionalSSAUseAndTypeList(operands) ||
       parseToken(Token::r_paren, "expected ')' to close argument list"))) {
    return failure();
  }

  return success();
}

/// Parse a comma-separated list of operation successors in brackets.
///
///   successor-list ::= `[` successor (`,` successor )* `]`
///
ParseResult FunctionParser::parseSuccessors(
    SmallVectorImpl<Block *> &destinations,
    SmallVectorImpl<SmallVector<Value *, 4>> &operands) {
  if (parseToken(Token::l_square, "expected '['"))
    return failure();

  auto parseElt = [this, &destinations, &operands]() {
    Block *dest;
    SmallVector<Value *, 4> destOperands;
    auto res = parseSuccessorAndUseList(dest, destOperands);
    destinations.push_back(dest);
    operands.push_back(destOperands);
    return res;
  };
  return parseCommaSeparatedListUntil(Token::r_square, parseElt,
                                      /*allowEmptyList=*/false);
}

namespace {
// RAII-style guard for cleaning up the regions in the operation state before
// deleting them.  Within the parser, regions may get deleted if parsing failed,
// and other errors may be present, in praticular undominated uses.  This makes
// sure such uses are deleted.
struct CleanupOpStateRegions {
  ~CleanupOpStateRegions() {
    SmallVector<Region *, 4> regionsToClean;
    regionsToClean.reserve(state.regions.size());
    for (auto &region : state.regions)
      if (region)
        for (auto &block : *region)
          block.dropAllDefinedValueUses();
  }
  OperationState &state;
};
} // namespace

Operation *FunctionParser::parseGenericOperation() {
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

  // Parse the region list.
  CleanupOpStateRegions guard{result};
  if (consumeIf(Token::l_paren)) {
    do {
      // Create temporary regions with function as parent.
      result.regions.emplace_back(new Region(function));
      if (parseRegion(*result.regions.back(),
                      /*entryArguments*/ {}))
        return nullptr;
    } while (consumeIf(Token::comma));
    if (parseToken(Token::r_paren, "expected ')' to end region list"))
      return nullptr;
  }

  if (getToken().is(Token::l_brace)) {
    if (parseAttributeDict(result.attributes))
      return nullptr;
  }

  if (parseToken(Token::colon, "expected ':' followed by operation type"))
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
    return (emitError(typeLoc, "expected ")
                << operandInfos.size() << " operand type" << plural
                << " but had " << operandTypes.size(),
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

  return builder.createOperation(result);
}

namespace {
class CustomOpAsmParser : public OpAsmParser {
public:
  CustomOpAsmParser(SMLoc nameLoc, StringRef opName, FunctionParser &parser)
      : nameLoc(nameLoc), opName(opName), parser(parser) {}

  ParseResult parseOperation(const AbstractOperation *opDefinition,
                             OperationState *opState) {
    if (opDefinition->parseAssembly(this, opState))
      return failure();

    // Check that none of the operands of the current operation reference an
    // entry block argument for any of the region.
    for (auto *entryArg : parsedRegionEntryArgumentPlaceholders)
      if (llvm::is_contained(opState->operands, entryArg))
        return emitError(nameLoc, "operand use before it's defined");

    return success();
  }

  //===--------------------------------------------------------------------===//
  // High level parsing methods.
  //===--------------------------------------------------------------------===//

  llvm::SMLoc getCurrentLocation() override {
    return parser.getToken().getLoc();
  }

  ParseResult parseComma() override {
    return parser.parseToken(Token::comma, "expected ','");
  }

  ParseResult parseOptionalComma() override {
    return success(parser.consumeIf(Token::comma));
  }

  ParseResult parseColon() override {
    return parser.parseToken(Token::colon, "expected ':'");
  }

  ParseResult parseOptionalColon() override {
    return success(parser.consumeIf(Token::colon));
  }

  ParseResult parseEqual() override {
    return parser.parseToken(Token::equal, "expected '='");
  }

  ParseResult parseType(Type &result) override {
    return failure(!(result = parser.parseType()));
  }

  ParseResult parseColonType(Type &result) override {
    return failure(parser.parseToken(Token::colon, "expected ':'") ||
                   !(result = parser.parseType()));
  }

  ParseResult parseColonTypeList(SmallVectorImpl<Type> &result) override {
    if (parser.parseToken(Token::colon, "expected ':'"))
      return failure();
    return parser.parseTypeListNoParens(result);
  }

  /// Parse an arrow followed by a type list, which must have at least one type.
  ParseResult parseOptionalArrowTypeList(SmallVectorImpl<Type> &result) {
    if (!parser.consumeIf(Token::arrow))
      return success();
    return parser.parseFunctionResultTypes(result);
  }

  ParseResult parseTrailingOperandList(SmallVectorImpl<OperandType> &result,
                                       int requiredOperandCount,
                                       Delimiter delimiter) override {
    if (parser.getToken().is(Token::comma)) {
      parseComma();
      return parseOperandList(result, requiredOperandCount, delimiter);
    }
    if (requiredOperandCount != -1)
      return emitError(parser.getToken().getLoc(), "expected ")
             << requiredOperandCount << " operands";
    return success();
  }

  /// Parse an optional keyword.
  ParseResult parseOptionalKeyword(const char *keyword) override {
    // Check that the current token is a bare identifier or keyword.
    if (parser.getToken().isNot(Token::bare_identifier) &&
        !parser.getToken().isKeyword())
      return failure();

    if (parser.getTokenSpelling() == keyword) {
      parser.consumeToken();
      return success();
    }
    return failure();
  }

  /// Parse an arbitrary attribute of a given type and return it in result. This
  /// also adds the attribute to the specified attribute list with the specified
  /// name.
  ParseResult parseAttribute(Attribute &result, Type type, StringRef attrName,
                             SmallVectorImpl<NamedAttribute> &attrs) override {
    result = parser.parseAttribute(type);
    if (!result)
      return failure();

    attrs.push_back(parser.builder.getNamedAttr(attrName, result));
    return success();
  }

  /// Parse an arbitrary attribute and return it in result.  This also adds
  /// the attribute to the specified attribute list with the specified name.
  ParseResult parseAttribute(Attribute &result, StringRef attrName,
                             SmallVectorImpl<NamedAttribute> &attrs) override {
    return parseAttribute(result, Type(), attrName, attrs);
  }

  /// If a named attribute list is present, parse is into result.
  ParseResult
  parseOptionalAttributeDict(SmallVectorImpl<NamedAttribute> &result) override {
    if (parser.getToken().isNot(Token::l_brace))
      return success();
    return parser.parseAttributeDict(result);
  }

  ParseResult parseOperand(OperandType &result) override {
    FunctionParser::SSAUseInfo useInfo;
    if (parser.parseSSAUse(useInfo))
      return failure();

    result = {useInfo.loc, useInfo.name, useInfo.number};
    return success();
  }

  ParseResult
  parseSuccessorAndUseList(Block *&dest,
                           SmallVectorImpl<Value *> &operands) override {
    // Defer successor parsing to the function parsers.
    return parser.parseSuccessorAndUseList(dest, operands);
  }

  ParseResult parseLParen() override {
    return parser.parseToken(Token::l_paren, "expected '('");
  }

  /// Parses a '(' if present.
  ParseResult parseOptionalLParen() override {
    return success(parser.consumeIf(Token::l_paren));
  }

  ParseResult parseRParen() override {
    return parser.parseToken(Token::r_paren, "expected ')'");
  }

  /// Parses a ')' if present.
  ParseResult parseOptionalRParen() override {
    return success(parser.consumeIf(Token::r_paren));
  }

  ParseResult parseOperandList(SmallVectorImpl<OperandType> &result,
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
        return success();
      LLVM_FALLTHROUGH;
    case Delimiter::Paren:
      if (parser.parseToken(Token::l_paren, "expected '(' in operand list"))
        return failure();
      break;
    case Delimiter::OptionalSquare:
      if (parser.getToken().isNot(Token::l_square))
        return success();
      LLVM_FALLTHROUGH;
    case Delimiter::Square:
      if (parser.parseToken(Token::l_square, "expected '[' in operand list"))
        return failure();
      break;
    }

    // Check for zero operands.
    if (parser.getToken().is(Token::percent_identifier)) {
      do {
        OperandType operand;
        if (parseOperand(operand))
          return failure();
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
        return failure();
      break;
    case Delimiter::OptionalSquare:
    case Delimiter::Square:
      if (parser.parseToken(Token::r_square, "expected ']' in operand list"))
        return failure();
      break;
    }

    if (requiredOperandCount != -1 &&
        result.size() != static_cast<size_t>(requiredOperandCount))
      return emitError(startLoc, "expected ")
             << requiredOperandCount << " operands";
    return success();
  }

  /// Parse a region that takes `arguments` of `argTypes` types.  This
  /// effectively defines the SSA values of `arguments` and assignes their type.
  ParseResult parseRegion(Region &region, ArrayRef<OperandType> arguments,
                          ArrayRef<Type> argTypes) override {
    assert(arguments.size() == argTypes.size() &&
           "mismatching number of arguments and types");

    SmallVector<std::pair<FunctionParser::SSAUseInfo, Type>, 2> regionArguments;
    for (const auto &pair : llvm::zip(arguments, argTypes)) {
      const OperandType &operand = std::get<0>(pair);
      Type type = std::get<1>(pair);
      FunctionParser::SSAUseInfo operandInfo = {operand.name, operand.number,
                                                operand.location};
      regionArguments.emplace_back(operandInfo, type);

      // Create a placeholder for this argument so that we can detect invalid
      // references to region arguments.
      Value *value = parser.resolveSSAUse(operandInfo, type);
      if (!value)
        return failure();
      parsedRegionEntryArgumentPlaceholders.emplace_back(value);
    }

    return parser.parseRegion(region, regionArguments);
  }

  /// Parses an optional region.
  ParseResult parseOptionalRegion(Region &region,
                                  ArrayRef<OperandType> arguments,
                                  ArrayRef<Type> argTypes) override {
    if (parser.getToken().isNot(Token::l_brace)) {
      if (!arguments.empty())
        return emitError(
            parser.getToken().getLoc(),
            "optional region with explicit entry arguments must be defined");
      return success();
    }
    return parseRegion(region, arguments, argTypes);
  }

  /// Parse a region argument.  Region arguments define new values, so this also
  /// checks if the values with the same name has not been defined yet.  The
  /// type of the argument will be resolved later by a call to `parseRegion`.
  ParseResult parseRegionArgument(OperandType &argument) override {
    // Use parseOperand to fill in the OperandType structure.
    if (parseOperand(argument))
      return failure();
    if (auto defLoc = parser.getDefinitionLoc(argument.name, argument.number)) {
      parser.emitError(argument.location,
                       "redefinition of SSA value '" + argument.name + "'")
              .attachNote(parser.getEncodedSourceLocation(*defLoc))
          << "previously defined here";
      return failure();
    }
    return success();
  }

  /// Parse an optional region argument.
  ParseResult parseOptionalRegionArgument(OperandType &argument) override {
    if (parser.getToken().isNot(Token::percent_identifier))
      return success();
    return parseRegionArgument(argument);
  }

  //===--------------------------------------------------------------------===//
  // Methods for interacting with the parser
  //===--------------------------------------------------------------------===//

  Builder &getBuilder() const override { return parser.builder; }

  llvm::SMLoc getNameLoc() const override { return nameLoc; }

  ParseResult resolveOperand(const OperandType &operand, Type type,
                             SmallVectorImpl<Value *> &result) override {
    FunctionParser::SSAUseInfo operandInfo = {operand.name, operand.number,
                                              operand.location};
    if (auto *value = parser.resolveSSAUse(operandInfo, type)) {
      result.push_back(value);
      return success();
    }
    return failure();
  }

  /// Emit a diagnostic at the specified location and return failure.
  InFlightDiagnostic emitError(llvm::SMLoc loc, const Twine &message) override {
    emittedError = true;
    return parser.emitError(loc, "custom op '" + opName + "' " + message);
  }

  bool didEmitError() const { return emittedError; }

private:
  SmallVector<Value *, 2> parsedRegionEntryArgumentPlaceholders;
  SMLoc nameLoc;
  StringRef opName;
  FunctionParser &parser;
  bool emittedError = false;
};
} // end anonymous namespace.

Operation *FunctionParser::parseCustomOperation() {
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
  CleanupOpStateRegions guard{opState};
  if (opAsmParser.parseOperation(opDefinition, &opState))
    return nullptr;

  // If it emitted an error, we failed.
  if (opAsmParser.didEmitError())
    return nullptr;

  // Otherwise, we succeeded.  Use the state it parsed as our op information.
  return builder.createOperation(opState);
}

//===----------------------------------------------------------------------===//
// Region Parsing
//===----------------------------------------------------------------------===//

/// Region.
///
///   region ::= '{' region-body
///
ParseResult FunctionParser::parseRegion(
    Region &region,
    ArrayRef<std::pair<FunctionParser::SSAUseInfo, Type>> entryArguments) {
  // Parse the '{'.
  if (parseToken(Token::l_brace, "expected '{' to begin a region"))
    return failure();

  // Check for an empty region.
  if (entryArguments.empty() && consumeIf(Token::r_brace))
    return success();
  auto currentPt = builder.saveInsertionPoint();

  // Push a new named value scope.
  pushSSANameScope();

  // Parse the first block directly to allow for it to be unnamed.
  Block *block = new Block();

  // Add arguments to the entry block.
  if (!entryArguments.empty()) {
    for (auto &placeholderArgPair : entryArguments)
      if (addDefinition(placeholderArgPair.first,
                        block->addArgument(placeholderArgPair.second))) {
        delete block;
        return failure();
      }

    // If we had named arguments, then don't allow a block name.
    if (getToken().is(Token::caret_identifier))
      return emitError("invalid block name in region with named arguments");
  }

  if (parseBlock(block)) {
    delete block;
    return failure();
  }

  // Verify that no other arguments were parsed.
  if (!entryArguments.empty() &&
      block->getNumArguments() > entryArguments.size()) {
    delete block;
    return emitError("entry block arguments were already defined");
  }

  // Parse the rest of the region.
  region.push_back(block);
  if (parseRegionBody(region))
    return failure();

  // Pop the SSA value scope for this region.
  if (popSSANameScope())
    return failure();

  // Reset the original insertion point.
  builder.restoreInsertionPoint(currentPt);
  return success();
}

/// Region.
///
///   region-body ::= block* '}'
///
ParseResult FunctionParser::parseRegionBody(Region &region) {
  // Parse the list of blocks.
  while (!consumeIf(Token::r_brace)) {
    Block *newBlock = nullptr;
    if (parseBlock(newBlock))
      return failure();
    region.push_back(newBlock);
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Block Parsing
//===----------------------------------------------------------------------===//

/// Block declaration.
///
///   block ::= block-label? operation*
///   block-label    ::= block-id block-arg-list? `:`
///   block-id       ::= caret-id
///   block-arg-list ::= `(` ssa-id-and-type-list? `)`
///
ParseResult FunctionParser::parseBlock(Block *&block) {
  // The first block of a region may already exist, if it does the caret
  // identifier is optional.
  if (block && getToken().isNot(Token::caret_identifier))
    return parseBlockBody(block);

  SMLoc nameLoc = getToken().getLoc();
  auto name = getTokenSpelling();
  if (parseToken(Token::caret_identifier, "expected block name"))
    return failure();

  block = defineBlockNamed(name, nameLoc, block);

  // Fail if the block was already defined.
  if (!block)
    return emitError(nameLoc, "redefinition of block '") << name << "'";

  // If an argument list is present, parse it.
  if (consumeIf(Token::l_paren)) {
    SmallVector<BlockArgument *, 8> bbArgs;
    if (parseOptionalBlockArgList(bbArgs, block) ||
        parseToken(Token::r_paren, "expected ')' to end argument list"))
      return failure();
  }

  if (parseToken(Token::colon, "expected ':' after block name"))
    return failure();

  return parseBlockBody(block);
}

ParseResult FunctionParser::parseBlockBody(Block *block) {
  // Set the insertion point to the end of the block to parse.
  builder.setInsertionPointToEnd(block);

  // Parse the list of operations that make up the body of the block.
  while (getToken().isNot(Token::caret_identifier, Token::r_brace))
    if (parseOperation())
      return failure();

  return success();
}

/// Get the block with the specified name, creating it if it doesn't already
/// exist.  The location specified is the point of use, which allows
/// us to diagnose references to blocks that are not defined precisely.
Block *FunctionParser::getBlockNamed(StringRef name, SMLoc loc) {
  auto &blockAndLoc = getBlockInfoByName(name);
  if (!blockAndLoc.first) {
    blockAndLoc = {new Block(), loc};
    insertForwardRef(blockAndLoc.first, loc);
  }

  return blockAndLoc.first;
}

/// Define the block with the specified name. Returns the Block* or nullptr in
/// the case of redefinition.
Block *FunctionParser::defineBlockNamed(StringRef name, SMLoc loc,
                                        Block *existing) {
  auto &blockAndLoc = getBlockInfoByName(name);
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
  if (!eraseForwardRef(blockAndLoc.first))
    return nullptr;
  return blockAndLoc.first;
}

/// Parse a (possibly empty) list of SSA operands with types as block arguments.
///
///   ssa-id-and-type-list ::= ssa-id-and-type (`,` ssa-id-and-type)*
///
ParseResult FunctionParser::parseOptionalBlockArgList(
    SmallVectorImpl<BlockArgument *> &results, Block *owner) {
  if (getToken().is(Token::r_brace))
    return success();

  // If the block already has arguments, then we're handling the entry block.
  // Parse and register the names for the arguments, but do not add them.
  bool definingExistingArgs = owner->getNumArguments() != 0;
  unsigned nextArgument = 0;

  return parseCommaSeparatedList([&]() -> ParseResult {
    return parseSSADefOrUseAndType(
        [&](SSAUseInfo useInfo, Type type) -> ParseResult {
          // If this block did not have existing arguments, define a new one.
          if (!definingExistingArgs)
            return addDefinition(useInfo, owner->addArgument(type));

          // Otherwise, ensure that this argument has already been created.
          if (nextArgument >= owner->getNumArguments())
            return emitError("too many arguments specified in argument list");

          // Finally, make sure the existing argument has the correct type.
          auto *arg = owner->getArgument(nextArgument++);
          if (arg->getType() != type)
            return emitError("argument and block argument type mismatch");
          return addDefinition(useInfo, arg);
        });
  });
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
  /// Parse an attribute alias declaration.
  ParseResult parseAttributeAliasDef();

  /// Parse an attribute alias declaration.
  ParseResult parseTypeAliasDef();

  // Functions.
  ParseResult
  parseArgumentList(SmallVectorImpl<Type> &argTypes,
                    SmallVectorImpl<std::pair<SMLoc, StringRef>> &argNames,
                    SmallVectorImpl<SmallVector<NamedAttribute, 2>> &argAttrs);
  ParseResult parseFunctionSignature(
      StringRef &name, FunctionType &type,
      SmallVectorImpl<std::pair<SMLoc, StringRef>> &argNames,
      SmallVectorImpl<SmallVector<NamedAttribute, 2>> &argAttrs);
  ParseResult parseFunc();
};
} // end anonymous namespace

/// Parses an attribute alias declaration.
///
///   attribute-alias-def ::= '#' alias-name `=` attribute-value
///
ParseResult ModuleParser::parseAttributeAliasDef() {
  assert(getToken().is(Token::hash_identifier));
  StringRef aliasName = getTokenSpelling().drop_front();

  // Check for redefinitions.
  if (getState().attributeAliasDefinitions.count(aliasName) > 0)
    return emitError("redefinition of attribute alias id '" + aliasName + "'");

  // Make sure this isn't invading the dialect attribute namespace.
  if (aliasName.contains('.'))
    return emitError("attribute names with a '.' are reserved for "
                     "dialect-defined names");

  consumeToken(Token::hash_identifier);

  // Parse the '='.
  if (parseToken(Token::equal, "expected '=' in attribute alias definition"))
    return failure();

  // Parse the attribute value.
  Attribute attr = parseAttribute();
  if (!attr)
    return failure();

  getState().attributeAliasDefinitions[aliasName] = attr;
  return success();
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

  // Make sure this isn't invading the dialect type namespace.
  if (aliasName.contains('.'))
    return emitError("type names with a '.' are reserved for "
                     "dialect-defined names");

  consumeToken(Token::exclamation_identifier);

  // Parse the '=' and 'type'.
  if (parseToken(Token::equal, "expected '=' in type alias definition") ||
      parseToken(Token::kw_type, "expected 'type' in type alias definition"))
    return failure();

  // Parse the type.
  Type aliasedType = parseType();
  if (!aliasedType)
    return failure();

  // Register this alias with the parser state.
  getState().typeAliasDefinitions.try_emplace(aliasName, aliasedType);
  return success();
}

/// Parse a (possibly empty) list of Function arguments with types.
///
///   named-argument ::= ssa-id `:` type attribute-dict?
///   argument-list  ::= named-argument (`,` named-argument)* | /*empty*/
///   argument-list ::= type attribute-dict? (`,` type attribute-dict?)*
///                     | /*empty*/
///
ParseResult ModuleParser::parseArgumentList(
    SmallVectorImpl<Type> &argTypes,
    SmallVectorImpl<std::pair<SMLoc, StringRef>> &argNames,
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

      argNames.emplace_back(loc, name);

      if (parseToken(Token::colon, "expected ':'"))
        return failure();
    } else {
      // Reject this if the preceding argument had a name.
      if (!argNames.empty())
        return emitError("expected SSA identifier");
    }

    // Parse argument type
    auto elt = parseType();
    if (!elt)
      return failure();
    argTypes.push_back(elt);

    // Parse the attribute dict.
    SmallVector<NamedAttribute, 2> attrs;
    if (getToken().is(Token::l_brace))
      if (parseAttributeDict(attrs))
        return failure();

    argAttrs.push_back(attrs);
    return success();
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
    StringRef &name, FunctionType &type,
    SmallVectorImpl<std::pair<SMLoc, StringRef>> &argNames,
    SmallVectorImpl<SmallVector<NamedAttribute, 2>> &argAttrs) {
  if (getToken().isNot(Token::at_identifier))
    return emitError("expected a function identifier like '@foo'");

  name = getTokenSpelling().drop_front();
  consumeToken(Token::at_identifier);

  if (getToken().isNot(Token::l_paren))
    return emitError("expected '(' in function signature");

  SmallVector<Type, 4> argTypes;
  if (parseArgumentList(argTypes, argNames, argAttrs))
    return failure();

  // Parse the return type if present.
  SmallVector<Type, 4> results;
  if (consumeIf(Token::arrow)) {
    if (parseFunctionResultTypes(results))
      return failure();
  }
  type = builder.getFunctionType(argTypes, results);
  return success();
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
  SmallVector<std::pair<SMLoc, StringRef>, 4> argNames;
  SmallVector<SmallVector<NamedAttribute, 2>, 4> argAttrs;

  auto loc = getToken().getLoc();
  if (parseFunctionSignature(name, type, argNames, argAttrs))
    return failure();

  // If function attributes are present, parse them.
  SmallVector<NamedAttribute, 8> attrs;
  if (consumeIf(Token::kw_attributes)) {
    if (parseAttributeDict(attrs))
      return failure();
  }

  // Okay, the function signature was parsed correctly, create the function now.
  auto *function =
      new Function(getEncodedSourceLocation(loc), name, type, attrs);
  getModule()->getFunctions().push_back(function);

  // Verify no name collision / redefinition.
  if (function->getName() != name)
    return emitError(loc, "redefinition of function named '") << name << "'";

  // Parse an optional trailing location.
  if (parseOptionalTrailingLocation(function))
    return failure();

  // Add the attributes to the function arguments.
  for (unsigned i = 0, e = function->getNumArguments(); i != e; ++i)
    function->setArgAttrs(i, argAttrs[i]);

  // External functions have no body.
  if (getToken().isNot(Token::l_brace))
    return success();
  auto braceLoc = getToken().getLoc();

  // Prepare the named function arguments.
  SmallVector<std::pair<FunctionParser::SSAUseInfo, Type>, 4> entryArgs;
  for (unsigned i = 0, e = argNames.size(); i != e; ++i) {
    entryArgs.emplace_back(
        FunctionParser::SSAUseInfo{argNames[i].second, 0, argNames[i].first},
        type.getInput(i));
  }

  // Parse the function body.
  auto parser = FunctionParser(getState(), function);
  if (parser.parseRegion(function->getBody(), entryArgs))
    return failure();

  // Verify that a valid function body was parsed.
  if (function->empty())
    return emitError(braceLoc, "function must have a body");

  return parser.finalize(braceLoc);
}

/// This is the top-level module parser.
ParseResult ModuleParser::parseModule() {
  while (1) {
    switch (getToken().getKind()) {
    default:
      emitError("expected a top level entity");
      return failure();

    // If we got to the end of the file, then we're done.
    case Token::eof:
      return success();

    // If we got an error token, then the lexer already emitted an error, just
    // stop.  Someday we could introduce error recovery if there was demand
    // for it.
    case Token::error:
      return failure();

    // Parse an attribute alias.
    case Token::hash_identifier:
      if (parseAttributeAliasDef())
        return failure();
      break;

    // Parse a type alias.
    case Token::exclamation_identifier:
      if (parseTypeAliasDef())
        return failure();
      break;

    case Token::kw_func:
      if (parseFunc())
        return failure();
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
  if (failed(module->verify()))
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
