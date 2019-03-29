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

BoolAttr::BoolAttr(Attribute::ImplType *ptr) : Attribute(ptr) {}

bool BoolAttr::getValue() const { return static_cast<ImplType *>(attr)->value; }

IntegerAttr::IntegerAttr(Attribute::ImplType *ptr) : Attribute(ptr) {}

APInt IntegerAttr::getValue() const {
  return static_cast<ImplType *>(attr)->getValue();
}

int64_t IntegerAttr::getInt() const { return getValue().getSExtValue(); }

FloatAttr::FloatAttr(Attribute::ImplType *ptr) : Attribute(ptr) {}

APFloat FloatAttr::getValue() const {
  return static_cast<ImplType *>(attr)->getValue();
}

double FloatAttr::getDouble() const { return getValue().convertToDouble(); }

StringAttr::StringAttr(Attribute::ImplType *ptr) : Attribute(ptr) {}

StringRef StringAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

ArrayAttr::ArrayAttr(Attribute::ImplType *ptr) : Attribute(ptr) {}

ArrayRef<Attribute> ArrayAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

AffineMapAttr::AffineMapAttr(Attribute::ImplType *ptr) : Attribute(ptr) {}

AffineMap AffineMapAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

IntegerSetAttr::IntegerSetAttr(Attribute::ImplType *ptr) : Attribute(ptr) {}

IntegerSet IntegerSetAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

TypeAttr::TypeAttr(Attribute::ImplType *ptr) : Attribute(ptr) {}

Type TypeAttr::getValue() const { return static_cast<ImplType *>(attr)->value; }

FunctionAttr::FunctionAttr(Attribute::ImplType *ptr) : Attribute(ptr) {}

Function *FunctionAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

FunctionType FunctionAttr::getType() const { return getValue()->getType(); }

ElementsAttr::ElementsAttr(Attribute::ImplType *ptr) : Attribute(ptr) {}

VectorOrTensorType ElementsAttr::getType() const {
  return static_cast<ImplType *>(attr)->type;
}

SplatElementsAttr::SplatElementsAttr(Attribute::ImplType *ptr)
    : ElementsAttr(ptr) {}

Attribute SplatElementsAttr::getValue() const {
  return static_cast<ImplType *>(attr)->elt;
}

DenseElementsAttr::DenseElementsAttr(Attribute::ImplType *ptr)
    : ElementsAttr(ptr) {}

void DenseElementsAttr::getValues(SmallVectorImpl<Attribute> &values) const {
  switch (getKind()) {
  case Attribute::Kind::DenseIntElements:
    cast<DenseIntElementsAttr>().getValues(values);
    return;
  case Attribute::Kind::DenseFPElements:
    cast<DenseFPElementsAttr>().getValues(values);
    return;
  default:
    llvm_unreachable("unexpected element type");
  }
}

ArrayRef<char> DenseElementsAttr::getRawData() const {
  return static_cast<ImplType *>(attr)->data;
}

DenseIntElementsAttr::DenseIntElementsAttr(Attribute::ImplType *ptr)
    : DenseElementsAttr(ptr) {}

/// Writes the lowest `bitWidth` bits of `value` to bit position `bitPos`
/// starting from `rawData`.
void DenseIntElementsAttr::writeBits(char *data, size_t bitPos, size_t bitWidth,
                                     uint64_t value) {
  // Read the destination bytes which will be written to.
  uint64_t dst = 0;
  auto dstData = reinterpret_cast<char *>(&dst);
  auto endPos = bitPos + bitWidth;
  auto start = data + bitPos / 8;
  auto end = data + endPos / 8 + (endPos % 8 != 0);
  std::copy(start, end, dstData);

  // Clean up the invalid bits in the destination bytes.
  dst &= ~(-1UL << (bitPos % 8));

  // Get the valid bits of the source value, shift them to right position,
  // then add them to the destination bytes.
  value <<= bitPos % 8;
  dst |= value;

  // Write the destination bytes back.
  ArrayRef<char> range({dstData, (size_t)(end - start)});
  std::copy(range.begin(), range.end(), start);
}

/// Reads the next `bitWidth` bits from the bit position `bitPos` of `rawData`
/// and put them in the lowest bits.
uint64_t DenseIntElementsAttr::readBits(const char *rawData, size_t bitPos,
                                        size_t bitsWidth) {
  uint64_t dst = 0;
  auto dstData = reinterpret_cast<char *>(&dst);
  auto endPos = bitPos + bitsWidth;
  auto start = rawData + bitPos / 8;
  auto end = rawData + endPos / 8 + (endPos % 8 != 0);
  std::copy(start, end, dstData);

  dst >>= bitPos % 8;
  dst &= ~(-1UL << bitsWidth);
  return dst;
}

void DenseIntElementsAttr::getValues(SmallVectorImpl<Attribute> &values) const {
  auto bitsWidth = static_cast<ImplType *>(attr)->bitsWidth;
  auto elementNum = getType().getNumElements();
  auto context = getType().getContext();
  values.reserve(elementNum);
  if (bitsWidth == 64) {
    ArrayRef<int64_t> vs(
        {reinterpret_cast<const int64_t *>(getRawData().data()),
         getRawData().size() / 8});
    for (auto value : vs) {
      auto attr = IntegerAttr::get(value, context);
      values.push_back(attr);
    }
  } else {
    const auto *rawData = getRawData().data();
    for (size_t pos = 0; pos < elementNum * bitsWidth; pos += bitsWidth) {
      uint64_t bits = readBits(rawData, pos, bitsWidth);
      APInt value(bitsWidth, bits, /*isSigned=*/true);
      auto attr = IntegerAttr::get(value.getSExtValue(), context);
      values.push_back(attr);
    }
  }
}

DenseFPElementsAttr::DenseFPElementsAttr(Attribute::ImplType *ptr)
    : DenseElementsAttr(ptr) {}

void DenseFPElementsAttr::getValues(SmallVectorImpl<Attribute> &values) const {
  auto elementNum = getType().getNumElements();
  auto context = getType().getContext();
  ArrayRef<double> vs({reinterpret_cast<const double *>(getRawData().data()),
                       getRawData().size() / 8});
  values.reserve(elementNum);
  for (auto v : vs) {
    auto attr = FloatAttr::get(v, context);
    values.push_back(attr);
  }
}

OpaqueElementsAttr::OpaqueElementsAttr(Attribute::ImplType *ptr)
    : ElementsAttr(ptr) {}

StringRef OpaqueElementsAttr::getValue() const {
  return static_cast<ImplType *>(attr)->bytes;
}

SparseElementsAttr::SparseElementsAttr(Attribute::ImplType *ptr)
    : ElementsAttr(ptr) {}

DenseIntElementsAttr SparseElementsAttr::getIndices() const {
  return static_cast<ImplType *>(attr)->indices;
}

DenseElementsAttr SparseElementsAttr::getValues() const {
  return static_cast<ImplType *>(attr)->values;
}
