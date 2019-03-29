//===- Attributes.cpp - MLIR Affine Expr Classes --------------------------===//
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

#include "mlir/IR/Attributes.h"
#include "AttributeDetail.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Types.h"

using namespace mlir;
using namespace mlir::detail;

Attribute::Kind Attribute::getKind() const { return attr->kind; }

bool Attribute::isOrContainsFunction() const {
  return attr->isOrContainsFunctionCache;
}

// Given an attribute that could refer to a function attribute in the remapping
// table, walk it and rewrite it to use the mapped function.  If it doesn't
// refer to anything in the table, then it is returned unmodified.
Attribute Attribute::remapFunctionAttrs(
    const llvm::DenseMap<Attribute, FunctionAttr> &remappingTable,
    MLIRContext *context) const {
  // Most attributes are trivially unrelated to function attributes, skip them
  // rapidly.
  if (!isOrContainsFunction())
    return *this;

  // If we have a function attribute, remap it.
  if (auto fnAttr = this->dyn_cast<FunctionAttr>()) {
    auto it = remappingTable.find(fnAttr);
    return it != remappingTable.end() ? it->second : *this;
  }

  // Otherwise, we must have an array attribute, remap the elements.
  auto arrayAttr = this->cast<ArrayAttr>();
  SmallVector<Attribute, 8> remappedElts;
  bool anyChange = false;
  for (auto elt : arrayAttr.getValue()) {
    auto newElt = elt.remapFunctionAttrs(remappingTable, context);
    remappedElts.push_back(newElt);
    anyChange |= (elt != newElt);
  }

  if (!anyChange)
    return *this;

  return ArrayAttr::get(remappedElts, context);
}

/// BoolAttr

bool BoolAttr::getValue() const { return static_cast<ImplType *>(attr)->value; }

/// IntegerAttr

APInt IntegerAttr::getValue() const {
  return static_cast<ImplType *>(attr)->getValue();
}

int64_t IntegerAttr::getInt() const { return getValue().getSExtValue(); }

Type IntegerAttr::getType() const {
  return static_cast<ImplType *>(attr)->type;
}

/// FloatAttr

APFloat FloatAttr::getValue() const {
  return static_cast<ImplType *>(attr)->getValue();
}

Type FloatAttr::getType() const { return static_cast<ImplType *>(attr)->type; }

double FloatAttr::getValueAsDouble() const {
  const auto &semantics = getType().cast<FloatType>().getFloatSemantics();
  auto value = getValue();
  bool losesInfo = false; // ignored
  if (&semantics != &APFloat::IEEEdouble()) {
    value.convert(APFloat::IEEEdouble(), APFloat::rmNearestTiesToEven,
                  &losesInfo);
  }
  return value.convertToDouble();
}

/// StringAttr

StringRef StringAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

/// ArrayAttr

ArrayRef<Attribute> ArrayAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

/// AffineMapAttr

AffineMap AffineMapAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

/// IntegerSetAttr

IntegerSet IntegerSetAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

/// TypeAttr

Type TypeAttr::getValue() const { return static_cast<ImplType *>(attr)->value; }

/// FunctionAttr

Function *FunctionAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

FunctionType FunctionAttr::getType() const { return getValue()->getType(); }

/// ElementsAttr

VectorOrTensorType ElementsAttr::getType() const {
  return static_cast<ImplType *>(attr)->type;
}

/// SplatElementsAttr

Attribute SplatElementsAttr::getValue() const {
  return static_cast<ImplType *>(attr)->elt;
}

/// DenseElementsAttr

void DenseElementsAttr::getValues(SmallVectorImpl<Attribute> &values) const {
  auto elementType = getType().getElementType();
  switch (getKind()) {
  case Attribute::Kind::DenseIntElements: {
    // Get the raw APInt values.
    SmallVector<APInt, 8> intValues;
    cast<DenseIntElementsAttr>().getValues(intValues);

    // Convert each to an IntegerAttr.
    for (auto &intVal : intValues)
      values.push_back(IntegerAttr::get(elementType, intVal));
    return;
  }
  case Attribute::Kind::DenseFPElements: {
    // Get the raw APFloat values.
    SmallVector<APFloat, 8> floatValues;
    cast<DenseFPElementsAttr>().getValues(floatValues);

    // Convert each to an FloatAttr.
    for (auto &floatVal : floatValues)
      values.push_back(FloatAttr::get(elementType, floatVal));
    return;
  }
  default:
    llvm_unreachable("unexpected element type");
  }
}

ArrayRef<char> DenseElementsAttr::getRawData() const {
  return static_cast<ImplType *>(attr)->data;
}

/// Parses the raw integer internal value for each dense element into
/// 'values'.
void DenseElementsAttr::getRawValues(SmallVectorImpl<APInt> &values) const {
  auto elementType = getType().getElementType();
  auto elementNum = getType().getNumElements();
  values.reserve(elementNum);

  // FIXME: using 64 bits for BF16 because it is currently stored with double
  // semantics.
  size_t bitWidth =
      elementType.isBF16() ? 64 : elementType.getIntOrFloatBitWidth();
  const auto *rawData = getRawData().data();
  for (size_t i = 0, e = elementNum; i != e; ++i)
    values.push_back(readBits(rawData, i * bitWidth, bitWidth));
}

/// Writes value to the bit position `bitPos` in array `rawData`. 'rawData' is
/// expected to be a 64-bit aligned storage address.
void DenseElementsAttr::writeBits(char *rawData, size_t bitPos, APInt value) {
  size_t bitWidth = value.getBitWidth();

  // If the bitwidth is 1 we just toggle the specific bit.
  if (bitWidth == 1) {
    auto *rawIntData = reinterpret_cast<uint64_t *>(rawData);
    if (value.isOneValue())
      APInt::tcSetBit(rawIntData, bitPos);
    else
      APInt::tcClearBit(rawIntData, bitPos);
    return;
  }

  // If the bit position and width are byte aligned, write the storage directly
  // to the data.
  if ((bitWidth % 8) == 0 && (bitPos % 8) == 0) {
    std::copy_n(reinterpret_cast<const char *>(value.getRawData()),
                bitWidth / 8, rawData + (bitPos / 8));
    return;
  }

  // Otherwise, convert the raw data into an APInt and insert the value at the
  // specified bit position.
  size_t totalWords = APInt::getNumWords((bitPos % 64) + bitWidth);
  llvm::MutableArrayRef<uint64_t> rawIntData(
      reinterpret_cast<uint64_t *>(rawData) + (bitPos / 64), totalWords);
  APInt tempStorage(totalWords * 64, rawIntData);
  tempStorage.insertBits(value, bitPos % 64);

  // Copy the value back to the raw data.
  std::copy_n(tempStorage.getRawData(), rawIntData.size(), rawIntData.data());
}

/// Reads the next `bitWidth` bits from the bit position `bitPos` in array
/// `rawData`. 'rawData' is expected to be a 64-bit aligned storage address.
APInt DenseElementsAttr::readBits(const char *rawData, size_t bitPos,
                                  size_t bitWidth) {
  // Reinterpret the raw data as a uint64_t word array and extract the value
  // starting at 'bitPos'.
  APInt result(bitWidth, 0);
  const uint64_t *intData = reinterpret_cast<const uint64_t *>(rawData);
  APInt::tcExtract(const_cast<uint64_t *>(result.getRawData()),
                   result.getNumWords(), intData, bitWidth, bitPos);
  return result;
}

/// DenseIntElementsAttr

void DenseIntElementsAttr::getValues(SmallVectorImpl<APInt> &values) const {
  // Simply return the raw integer values.
  getRawValues(values);
}

/// DenseFPElementsAttr

void DenseFPElementsAttr::getValues(SmallVectorImpl<APFloat> &values) const {
  // Get the raw APInt element values.
  SmallVector<APInt, 8> intValues;
  getRawValues(intValues);

  // Convert each of the APInt values to an APFloat.
  auto elementType = getType().getElementType().dyn_cast<FloatType>();
  const auto &elementSemantics = elementType.getFloatSemantics();
  for (auto &intValue : intValues)
    values.push_back(APFloat(elementSemantics, intValue));
}

/// OpaqueElementsAttr

StringRef OpaqueElementsAttr::getValue() const {
  return static_cast<ImplType *>(attr)->bytes;
}

/// SparseElementsAttr

DenseIntElementsAttr SparseElementsAttr::getIndices() const {
  return static_cast<ImplType *>(attr)->indices;
}

DenseElementsAttr SparseElementsAttr::getValues() const {
  return static_cast<ImplType *>(attr)->values;
}
