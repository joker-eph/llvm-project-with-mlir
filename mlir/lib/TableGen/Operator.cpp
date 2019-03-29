//===- Operator.cpp - Operator class --------------------------------------===//
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
// Operator wrapper to simplify using TableGen Record defining a MLIR Op.
//
//===----------------------------------------------------------------------===//

#include "mlir/TableGen/Operator.h"
#include "mlir/TableGen/Predicate.h"
#include "mlir/TableGen/Type.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"

using namespace mlir;

using llvm::DagInit;
using llvm::DefInit;
using llvm::Record;

tblgen::Operator::Operator(const llvm::Record &def) : def(def) {
  SplitString(def.getName(), splittedDefName, "_");
  populateOperandsAndAttributes();
}

const SmallVectorImpl<StringRef> &tblgen::Operator::getSplitDefName() const {
  return splittedDefName;
}

StringRef tblgen::Operator::getOperationName() const {
  return def.getValueAsString("opName");
}

StringRef tblgen::Operator::cppClassName() const {
  return getSplitDefName().back();
}
std::string tblgen::Operator::qualifiedCppClassName() const {
  return llvm::join(getSplitDefName(), "::");
}

int tblgen::Operator::getNumNativeAttributes() const {
  return derivedAttrStart - nativeAttrStart;
}

const tblgen::NamedAttribute &tblgen::Operator::getAttribute(int index) const {
  return attributes[index];
}

StringRef tblgen::Operator::getArgName(int index) const {
  DagInit *argumentValues = def.getValueAsDag("arguments");
  return argumentValues->getArgName(index)->getValue();
}

auto tblgen::Operator::attribute_begin() -> attribute_iterator {
  return attributes.begin();
}
auto tblgen::Operator::attribute_end() -> attribute_iterator {
  return attributes.end();
}
auto tblgen::Operator::getAttributes()
    -> llvm::iterator_range<attribute_iterator> {
  return {attribute_begin(), attribute_end()};
}

auto tblgen::Operator::operand_begin() -> operand_iterator {
  return operands.begin();
}
auto tblgen::Operator::operand_end() -> operand_iterator {
  return operands.end();
}
auto tblgen::Operator::getOperands() -> llvm::iterator_range<operand_iterator> {
  return {operand_begin(), operand_end()};
}

auto tblgen::Operator::getArg(int index) -> Argument {
  if (index < nativeAttrStart)
    return {&operands[index]};
  return {&attributes[index - nativeAttrStart]};
}

void tblgen::Operator::populateOperandsAndAttributes() {
  auto &recordKeeper = def.getRecords();
  auto attrClass = recordKeeper.getClass("Attr");
  auto derivedAttrClass = recordKeeper.getClass("DerivedAttr");
  derivedAttrStart = -1;

  // The argument ordering is operands, native attributes, derived
  // attributes.
  DagInit *argumentValues = def.getValueAsDag("arguments");
  unsigned i = 0;
  // Handle operands.
  for (unsigned e = argumentValues->getNumArgs(); i != e; ++i) {
    auto arg = argumentValues->getArg(i);
    auto givenName = argumentValues->getArgName(i);
    auto argDefInit = dyn_cast<DefInit>(arg);
    if (!argDefInit)
      PrintFatalError(def.getLoc(),
                      Twine("undefined type for argument ") + Twine(i));
    Record *argDef = argDefInit->getDef();
    if (argDef->isSubClassOf(attrClass))
      break;
    operands.push_back(Operand{givenName, argDefInit});
  }

  // Handle native attributes.
  nativeAttrStart = i;
  for (unsigned e = argumentValues->getNumArgs(); i != e; ++i) {
    auto arg = argumentValues->getArg(i);
    auto givenName = argumentValues->getArgName(i);
    Record *argDef = cast<DefInit>(arg)->getDef();
    if (!argDef->isSubClassOf(attrClass))
      PrintFatalError(def.getLoc(),
                      Twine("expected attribute as argument ") + Twine(i));

    if (!givenName)
      PrintFatalError(argDef->getLoc(), "attributes must be named");
    bool isDerived = argDef->isSubClassOf(derivedAttrClass);
    if (isDerived)
      PrintFatalError(def.getLoc(),
                      "derived attributes not allowed in argument list");
    attributes.push_back({givenName, Attribute(argDef)});
  }

  // Handle derived attributes.
  derivedAttrStart = i;
  for (const auto &val : def.getValues()) {
    if (auto *record = dyn_cast<llvm::RecordRecTy>(val.getType())) {
      if (!record->isSubClassOf(attrClass))
        continue;
      if (!record->isSubClassOf(derivedAttrClass))
        PrintFatalError(def.getLoc(),
                        "unexpected Attr where only DerivedAttr is allowed");

      if (record->getClasses().size() != 1) {
        PrintFatalError(
            def.getLoc(),
            "unsupported attribute modelling, only single class expected");
      }
      attributes.push_back({cast<llvm::StringInit>(val.getNameInit()),
                            Attribute(cast<DefInit>(val.getValue()))});
    }
  }
}

bool tblgen::Operator::hasDescription() const {
  return def.getValue("description") != nullptr;
}

StringRef tblgen::Operator::getDescription() const {
  return def.getValueAsString("description");
}

bool tblgen::Operator::hasSummary() const {
  return def.getValue("summary") != nullptr;
}

StringRef tblgen::Operator::getSummary() const {
  return def.getValueAsString("summary");
}
