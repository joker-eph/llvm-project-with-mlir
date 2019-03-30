//===- Pattern.cpp - Pattern wrapper class ----------------------*- C++ -*-===//
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
// Pattern wrapper class to simplify using TableGen Record defining a MLIR
// Pattern.
//
//===----------------------------------------------------------------------===//

#include "mlir/TableGen/Pattern.h"
#include "llvm/ADT/Twine.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"

using namespace mlir;

using mlir::tblgen::Operator;

bool tblgen::DagLeaf::isUnspecified() const {
  return !def || isa<llvm::UnsetInit>(def);
}

bool tblgen::DagLeaf::isOperandMatcher() const {
  if (!def || !isa<llvm::DefInit>(def))
    return false;
  // Operand matchers specify a type constraint.
  return cast<llvm::DefInit>(def)->getDef()->isSubClassOf("TypeConstraint");
}

bool tblgen::DagLeaf::isAttrMatcher() const {
  if (!def || !isa<llvm::DefInit>(def))
    return false;
  // Attribute matchers specify an attribute constraint.
  return cast<llvm::DefInit>(def)->getDef()->isSubClassOf("AttrConstraint");
}

bool tblgen::DagLeaf::isAttrTransformer() const {
  if (!def || !isa<llvm::DefInit>(def))
    return false;
  return cast<llvm::DefInit>(def)->getDef()->isSubClassOf("tAttr");
}

bool tblgen::DagLeaf::isConstantAttr() const {
  if (!def || !isa<llvm::DefInit>(def))
    return false;
  return cast<llvm::DefInit>(def)->getDef()->isSubClassOf("ConstantAttr");
}

tblgen::Constraint tblgen::DagLeaf::getAsConstraint() const {
  assert((isOperandMatcher() || isAttrMatcher()) &&
         "the DAG leaf must be operand or attribute");
  return Constraint(cast<llvm::DefInit>(def)->getDef());
}

tblgen::ConstantAttr tblgen::DagLeaf::getAsConstantAttr() const {
  assert(isConstantAttr() && "the DAG leaf must be constant attribute");
  return ConstantAttr(cast<llvm::DefInit>(def));
}

std::string tblgen::DagLeaf::getConditionTemplate() const {
  return getAsConstraint().getConditionTemplate();
}

std::string tblgen::DagLeaf::getTransformationTemplate() const {
  assert(isAttrTransformer() && "the DAG leaf must be attribute transformer");
  return cast<llvm::DefInit>(def)
      ->getDef()
      ->getValueAsString("attrTransform")
      .str();
}

bool tblgen::DagNode::isAttrTransformer() const {
  auto op = node->getOperator();
  if (!op || !isa<llvm::DefInit>(op))
    return false;
  return cast<llvm::DefInit>(op)->getDef()->isSubClassOf("tAttr");
}

std::string tblgen::DagNode::getTransformationTemplate() const {
  assert(isAttrTransformer() && "the DAG leaf must be attribute transformer");
  return cast<llvm::DefInit>(node->getOperator())
      ->getDef()
      ->getValueAsString("attrTransform")
      .str();
}

llvm::StringRef tblgen::DagNode::getOpName() const {
  return node->getNameStr();
}

Operator &tblgen::DagNode::getDialectOp(RecordOperatorMap *mapper) const {
  llvm::Record *opDef = cast<llvm::DefInit>(node->getOperator())->getDef();
  return mapper->try_emplace(opDef, opDef).first->second;
}

unsigned tblgen::DagNode::getNumOps() const {
  unsigned count = isReplaceWithValue() ? 0 : 1;
  for (unsigned i = 0, e = getNumArgs(); i != e; ++i) {
    if (auto child = getArgAsNestedDag(i))
      count += child.getNumOps();
  }
  return count;
}

unsigned tblgen::DagNode::getNumArgs() const { return node->getNumArgs(); }

bool tblgen::DagNode::isNestedDagArg(unsigned index) const {
  return isa<llvm::DagInit>(node->getArg(index));
}

tblgen::DagNode tblgen::DagNode::getArgAsNestedDag(unsigned index) const {
  return DagNode(dyn_cast_or_null<llvm::DagInit>(node->getArg(index)));
}

tblgen::DagLeaf tblgen::DagNode::getArgAsLeaf(unsigned index) const {
  assert(!isNestedDagArg(index));
  return DagLeaf(node->getArg(index));
}

StringRef tblgen::DagNode::getArgName(unsigned index) const {
  return node->getArgNameStr(index);
}

static void collectBoundArguments(const llvm::DagInit *tree,
                                  tblgen::Pattern *pattern) {
  auto &op = pattern->getDialectOp(tblgen::DagNode(tree));
  if (llvm::StringInit *si = tree->getName()) {
    auto name = si->getAsUnquotedString();
    if (!name.empty())
      pattern->getSourcePatternBoundResults().insert(name);
  }

  // TODO(jpienaar): Expand to multiple matches.
  for (unsigned i = 0, e = tree->getNumArgs(); i != e; ++i) {
    auto *arg = tree->getArg(i);

    if (auto *argTree = dyn_cast<llvm::DagInit>(arg)) {
      collectBoundArguments(argTree, pattern);
      continue;
    }

    StringRef name = tree->getArgNameStr(i);
    if (name.empty())
      continue;

    pattern->getSourcePatternBoundArgs().try_emplace(name, op.getArg(i));
  }
}

void tblgen::DagNode::collectBoundArguments(tblgen::Pattern *pattern) const {
  ::collectBoundArguments(node, pattern);
}

bool tblgen::DagNode::isReplaceWithValue() const {
  auto *dagOpDef = cast<llvm::DefInit>(node->getOperator())->getDef();
  return dagOpDef->getName() == "replaceWithValue";
}

bool tblgen::DagNode::isVerifyUnusedValue() const {
  auto *dagOpDef = cast<llvm::DefInit>(node->getOperator())->getDef();
  return dagOpDef->getName() == "verifyUnusedValue";
}

bool tblgen::DagNode::isNativeCodeBuilder() const {
  auto *dagOpDef = cast<llvm::DefInit>(node->getOperator())->getDef();
  return dagOpDef->isSubClassOf("cOp");
}

llvm::StringRef tblgen::DagNode::getNativeCodeBuilder() const {
  assert(isNativeCodeBuilder());
  auto *dagOpDef = cast<llvm::DefInit>(node->getOperator())->getDef();
  return dagOpDef->getValueAsString("function");
}

tblgen::Pattern::Pattern(const llvm::Record *def, RecordOperatorMap *mapper)
    : def(*def), recordOpMap(mapper) {
  getSourcePattern().collectBoundArguments(this);
}

tblgen::DagNode tblgen::Pattern::getSourcePattern() const {
  return tblgen::DagNode(def.getValueAsDag("sourcePattern"));
}

unsigned tblgen::Pattern::getNumResults() const {
  auto *results = def.getValueAsListInit("resultPatterns");
  return results->size();
}

tblgen::DagNode tblgen::Pattern::getResultPattern(unsigned index) const {
  auto *results = def.getValueAsListInit("resultPatterns");
  return tblgen::DagNode(cast<llvm::DagInit>(results->getElement(index)));
}

bool tblgen::Pattern::isArgBoundInSourcePattern(llvm::StringRef name) const {
  return boundArguments.find(name) != boundArguments.end();
}

bool tblgen::Pattern::isResultBoundInSourcePattern(llvm::StringRef name) const {
  return boundResults.count(name);
}

void tblgen::Pattern::ensureArgBoundInSourcePattern(
    llvm::StringRef name) const {
  if (!isArgBoundInSourcePattern(name))
    PrintFatalError(def.getLoc(),
                    Twine("referencing unbound variable '") + name + "'");
}

llvm::StringMap<tblgen::Argument> &
tblgen::Pattern::getSourcePatternBoundArgs() {
  return boundArguments;
}

llvm::StringSet<> &tblgen::Pattern::getSourcePatternBoundResults() {
  return boundResults;
}

const tblgen::Operator &tblgen::Pattern::getSourceRootOp() {
  return getSourcePattern().getDialectOp(recordOpMap);
}

tblgen::Operator &tblgen::Pattern::getDialectOp(DagNode node) {
  return node.getDialectOp(recordOpMap);
}

std::vector<tblgen::AppliedConstraint> tblgen::Pattern::getConstraints() const {
  auto *listInit = def.getValueAsListInit("constraints");
  std::vector<tblgen::AppliedConstraint> ret;
  ret.reserve(listInit->size());

  for (auto it : *listInit) {
    auto *dagInit = dyn_cast<llvm::DagInit>(it);
    if (!dagInit)
      PrintFatalError(def.getLoc(), "all elemements in Pattern multi-entity "
                                    "constraints should be DAG nodes");

    std::vector<std::string> entities;
    entities.reserve(dagInit->arg_size());
    for (auto *argName : dagInit->getArgNames())
      entities.push_back(argName->getValue());

    ret.emplace_back(cast<llvm::DefInit>(dagInit->getOperator())->getDef(),
                     std::move(entities));
  }
  return ret;
}

int tblgen::Pattern::getBenefit() const {
  // The default benefit value is a heristic with number of ops in the source
  // pattern.
  const int defaultBenefit = getSourcePattern().getNumOps();
  llvm::DagInit *delta = def.getValueAsDag("benefitDelta");
  if (delta->getNumArgs() != 1 || !isa<llvm::IntInit>(delta->getArg(0)))
    PrintFatalError(def.getLoc(), "The 'AddedBenefit' can only be an integer");
  return defaultBenefit + dyn_cast<llvm::IntInit>(delta->getArg(0))->getValue();
}
