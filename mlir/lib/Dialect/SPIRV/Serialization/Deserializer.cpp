//===- Deserializer.cpp - MLIR SPIR-V Deserialization ---------------------===//
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
// This file defines the SPIR-V binary to MLIR SPIR-V module deseralization.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/SPIRV/Serialization.h"

#include "SPIRVBinaryUtils.h"
#include "mlir/Dialect/SPIRV/SPIRVOps.h"
#include "mlir/Dialect/SPIRV/SPIRVTypes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Location.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/SmallVector.h"

using namespace mlir;

template <typename Dst, typename Src>
inline Dst bitwiseCast(Src source) noexcept {
  Dst dest;
  static_assert(sizeof(source) == sizeof(dest),
                "bitwiseCast requires same source and destination bitwidth");
  std::memcpy(&dest, &source, sizeof(dest));
  return dest;
}

namespace {
/// A SPIR-V module serializer.
///
/// A SPIR-V binary module is a single linear stream of instructions; each
/// instruction is composed of 32-bit words. The first word of an instruction
/// records the total number of words of that instruction using the 16
/// higher-order bits. So this deserializer uses that to get instruction
/// boundary and parse instructions and build a SPIR-V ModuleOp gradually.
///
// TODO(antiagainst): clean up created ops on errors
class Deserializer {
public:
  /// Creates a deserializer for the given SPIR-V `binary` module.
  /// The SPIR-V ModuleOp will be created into `context.
  explicit Deserializer(ArrayRef<uint32_t> binary, MLIRContext *context);

  /// Deserializes the remembered SPIR-V binary module.
  LogicalResult deserialize();

  /// Collects the final SPIR-V ModuleOp.
  Optional<spirv::ModuleOp> collect();

private:
  //===--------------------------------------------------------------------===//
  // Module structure
  //===--------------------------------------------------------------------===//

  /// Initializes the `module` ModuleOp in this deserializer instance.
  spirv::ModuleOp createModuleOp();

  /// Processes SPIR-V module header in `binary`.
  LogicalResult processHeader();

  /// Processes the SPIR-V OpMemoryModel with `operands` and updates `module`.
  LogicalResult processMemoryModel(ArrayRef<uint32_t> operands);

  /// Processes the SPIR-V function at the current `offset` into `binary`.
  /// The operands to the OpFunction instruction is passed in as ``operands`.
  /// This method processes each instruction inside the function and dispatches
  /// them to their handler method accordingly.
  LogicalResult processFunction(ArrayRef<uint32_t> operands);

  //===--------------------------------------------------------------------===//
  // Type
  //===--------------------------------------------------------------------===//

  /// Gets type for a given result <id>.
  Type getType(uint32_t id) { return typeMap.lookup(id); }

  /// Returns true if the given `type` is for SPIR-V void type.
  bool isVoidType(Type type) const { return type.isa<NoneType>(); }

  /// Processes a SPIR-V type instruction with given `opcode` and `operands` and
  /// registers the type into `module`.
  LogicalResult processType(spirv::Opcode opcode, ArrayRef<uint32_t> operands);

  LogicalResult processFunctionType(ArrayRef<uint32_t> operands);

  //===--------------------------------------------------------------------===//
  // Instruction
  //===--------------------------------------------------------------------===//

  /// Get the Value associated with a result <id>.
  Value *getValue(uint32_t id) { return valueMap.lookup(id); }

  /// Slices the first instruction out of `binary` and returns its opcode and
  /// operands via `opcode` and `operands` respectively.
  LogicalResult sliceInstruction(spirv::Opcode &opcode,
                                 ArrayRef<uint32_t> &operands);

  /// Processes a SPIR-V instruction with the given `opcode` and `operands`.
  /// This method is the main entrance for handling SPIR-V instruction; it
  /// checks the instruction opcode and dispatches to the corresponding handler.
  LogicalResult processInstruction(spirv::Opcode opcode,
                                   ArrayRef<uint32_t> operands);

  /// Method to dispatch to the specialized deserialization function for an
  /// operation in SPIR-V dialect that is a mirror of an instruction in the
  /// SPIR-V spec. This is auto-generated from ODS. Dispatch is handled for
  /// all operations in SPIR-V dialect that have hasOpcode == 1.
  LogicalResult dispatchToAutogenDeserialization(spirv::Opcode opcode,
                                                 ArrayRef<uint32_t> words);

  /// Method to deserialize an operation in the SPIR-V dialect that is a mirror
  /// of an instruction in the SPIR-V spec. This is auto generated if hasOpcode
  /// == 1 and autogenSerialization == 1 in ODS.
  template <typename OpTy> LogicalResult processOp(ArrayRef<uint32_t> words) {
    return processOpImpl<OpTy>(words);
  }

  template <typename OpTy>
  LogicalResult processOpImpl(ArrayRef<uint32_t> words) {
    return emitError(unknownLoc, "unsupported deserialization for ")
           << OpTy::getOperationName() << " op";
  }

private:
  /// The SPIR-V binary module.
  ArrayRef<uint32_t> binary;

  /// The current word offset into the binary module.
  unsigned curOffset = 0;

  /// MLIRContext to create SPIR-V ModuleOp into.
  MLIRContext *context;

  // TODO(antiagainst): create Location subclass for binary blob
  Location unknownLoc;

  /// The SPIR-V ModuleOp.
  Optional<spirv::ModuleOp> module;

  OpBuilder opBuilder;

  // result <id> to type mapping
  DenseMap<uint32_t, Type> typeMap;

  // result <id> to function mapping
  DenseMap<uint32_t, Operation *> funcMap;

  // result <id> to value mapping
  DenseMap<uint32_t, Value *> valueMap;
};
} // namespace

Deserializer::Deserializer(ArrayRef<uint32_t> binary, MLIRContext *context)
    : binary(binary), context(context), unknownLoc(UnknownLoc::get(context)),
      module(createModuleOp()),
      opBuilder(module->getOperation()->getRegion(0)) {}

LogicalResult Deserializer::deserialize() {
  if (failed(processHeader()))
    return failure();

  spirv::Opcode opcode;
  ArrayRef<uint32_t> operands;
  while (succeeded(sliceInstruction(opcode, operands))) {
    if (failed(processInstruction(opcode, operands)))
      return failure();
  }

  return success();
}

Optional<spirv::ModuleOp> Deserializer::collect() { return module; }

//===----------------------------------------------------------------------===//
// Module structure
//===----------------------------------------------------------------------===//

spirv::ModuleOp Deserializer::createModuleOp() {
  Builder builder(context);
  OperationState state(unknownLoc, spirv::ModuleOp::getOperationName());
  // TODO(antiagainst): use target environment to select the version
  state.addAttribute("major_version", builder.getI32IntegerAttr(1));
  state.addAttribute("minor_version", builder.getI32IntegerAttr(0));
  spirv::ModuleOp::build(&builder, &state);
  return llvm::cast<spirv::ModuleOp>(Operation::create(state));
}

LogicalResult Deserializer::processHeader() {
  if (binary.size() < spirv::kHeaderWordCount)
    return emitError(unknownLoc,
                     "SPIR-V binary module must have a 5-word header");

  if (binary[0] != spirv::kMagicNumber)
    return emitError(unknownLoc, "incorrect magic number");

  // TODO(antiagainst): generator number, bound, schema
  curOffset = spirv::kHeaderWordCount;
  return success();
}

LogicalResult Deserializer::processMemoryModel(ArrayRef<uint32_t> operands) {
  if (operands.size() != 2)
    return emitError(unknownLoc, "OpMemoryModel must have two operands");

  module->setAttr(
      "addressing_model",
      opBuilder.getI32IntegerAttr(bitwiseCast<int32_t>(operands.front())));
  module->setAttr("memory_model", opBuilder.getI32IntegerAttr(
                                      bitwiseCast<int32_t>(operands.back())));

  return success();
}

LogicalResult Deserializer::processFunction(ArrayRef<uint32_t> operands) {
  // Get the result type
  if (operands.size() != 4) {
    return emitError(unknownLoc, "OpFunction must have 4 parameters");
  }
  Type resultType = getType(operands[0]);
  if (!resultType) {
    return emitError(unknownLoc, "unknown result type from <id> ")
           << operands[0];
  }
  if (funcMap.count(operands[1])) {
    return emitError(unknownLoc, "duplicate function definition/declaration");
  }
  auto functionControl = spirv::symbolizeFunctionControl(operands[2]);
  if (!functionControl) {
    return emitError(unknownLoc, "unknown Function Control : ") << operands[2];
  }
  if (functionControl.getValue() != spirv::FunctionControl::None) {
    /// TODO : Handle different function controls
    return emitError(unknownLoc, "unhandled Function Control : '")
           << spirv::stringifyFunctionControl(functionControl.getValue())
           << "'";
  }
  Type fnType = getType(operands[3]);
  if (!fnType || !fnType.isa<FunctionType>()) {
    return emitError(unknownLoc, "unknown function type from <id> ")
           << operands[3];
  }
  auto functionType = fnType.cast<FunctionType>();
  if ((isVoidType(resultType) && functionType.getNumResults() != 0) ||
      (functionType.getNumResults() == 1 &&
       functionType.getResult(0) != resultType)) {
    return emitError(unknownLoc, "mismatch in function type ")
           << functionType << " and return type " << resultType << " specified";
  }
  /// TODO : The function name must be obtained from OpName eventually
  std::string fnName = "spirv_fn_" + std::to_string(operands[2]);
  auto funcOp = opBuilder.create<FuncOp>(unknownLoc, fnName, functionType,
                                         ArrayRef<NamedAttribute>());
  funcOp.addEntryBlock();

  // Parse the op argument instructions
  if (functionType.getNumInputs()) {
    for (size_t i = 0, e = functionType.getNumInputs(); i != e; ++i) {
      auto argType = functionType.getInput(i);
      spirv::Opcode opcode;
      ArrayRef<uint32_t> operands;
      if (failed(sliceInstruction(opcode, operands))) {
        return failure();
      }
      if (opcode != spirv::Opcode::OpFunctionParameter) {
        return emitError(
                   unknownLoc,
                   "missing OpFunctionParameter instruction for argument ")
               << i;
      }
      if (operands.size() != 2) {
        return emitError(
            unknownLoc,
            "expected result type and result <id> for OpFunctionParameter");
      }
      auto argDefinedType = getType(operands[0]);
      if (!argDefinedType || argDefinedType != argType) {
        return emitError(unknownLoc,
                         "mismatch in argument type between function type "
                         "definition ")
               << functionType << " and argument type definition "
               << argDefinedType << " at argument " << i;
      }
      if (getValue(operands[1])) {
        return emitError(unknownLoc, "duplicate definition of result <id> '")
               << operands[1];
      }
      auto argValue = funcOp.getArgument(i);
      valueMap[operands[1]] = argValue;
    }
  }

  // Create a new builder for building the body
  OpBuilder funcBody(funcOp.getBody());
  std::swap(funcBody, opBuilder);

  spirv::Opcode opcode;
  ArrayRef<uint32_t> instOperands;
  while (succeeded(sliceInstruction(opcode, instOperands)) &&
         opcode != spirv::Opcode::OpFunctionEnd) {
    if (failed(processInstruction(opcode, instOperands))) {
      return failure();
    }
  }
  std::swap(funcBody, opBuilder);
  if (opcode != spirv::Opcode::OpFunctionEnd) {
    return failure();
  }
  if (!instOperands.empty()) {
    return emitError(unknownLoc, "unexpected operands for OpFunctionEnd");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Type
//===----------------------------------------------------------------------===//

LogicalResult Deserializer::processType(spirv::Opcode opcode,
                                        ArrayRef<uint32_t> operands) {
  if (operands.empty()) {
    return emitError(unknownLoc, "type instruction with opcode ")
           << spirv::stringifyOpcode(opcode) << " needs at least one <id>";
  }
  /// TODO: Types might be forward declared in some instructions and need to be
  /// handled appropriately.
  if (typeMap.count(operands[0])) {
    return emitError(unknownLoc, "duplicate definition for result <id> ")
           << operands[0];
  }
  switch (opcode) {
  case spirv::Opcode::OpTypeVoid:
    if (operands.size() != 1) {
      return emitError(unknownLoc, "OpTypeVoid must have no parameters");
    }
    typeMap[operands[0]] = NoneType::get(context);
    break;
  case spirv::Opcode::OpTypeFloat: {
    if (operands.size() != 2) {
      return emitError(unknownLoc, "OpTypeFloat must have bitwidth parameter");
    }
    Type floatTy;
    switch (operands[1]) {
    case 16:
      floatTy = opBuilder.getF16Type();
      break;
    case 32:
      floatTy = opBuilder.getF32Type();
      break;
    case 64:
      floatTy = opBuilder.getF64Type();
      break;
    default:
      return emitError(unknownLoc, "unsupported bitwdith ")
             << operands[1] << " with OpTypeFloat";
    }
    typeMap[operands[0]] = floatTy;
  } break;
  case spirv::Opcode::OpTypePointer: {
    if (operands.size() != 3) {
      return emitError(unknownLoc, "OpTypePointer must have two parameters");
    }
    auto pointeeType = getType(operands[2]);
    if (!pointeeType) {
      return emitError(unknownLoc, "unknown OpTypePointer pointee type <id> : ")
             << operands[2];
    }
    auto storageClass = static_cast<spirv::StorageClass>(operands[1]);
    typeMap[operands[0]] = spirv::PointerType::get(pointeeType, storageClass);
  } break;
  case spirv::Opcode::OpTypeFunction:
    return processFunctionType(operands);
  default:
    return emitError(unknownLoc, "unhandled type instruction");
  }
  return success();
}

LogicalResult Deserializer::processFunctionType(ArrayRef<uint32_t> operands) {
  assert(!operands.empty() && "No operands for processing function type");
  if (operands.size() == 1) {
    return emitError(unknownLoc, "missing return type for OpTypeFunction");
  }
  auto returnType = getType(operands[1]);
  if (!returnType) {
    return emitError(unknownLoc, "unknown return type in OpTypeFunction");
  }
  SmallVector<Type, 1> argTypes;
  for (size_t i = 2, e = operands.size(); i < e; ++i) {
    auto ty = getType(operands[i]);
    if (!ty) {
      return emitError(unknownLoc, "unknown argument type in OpTypeFunction");
    }
    argTypes.push_back(ty);
  }
  ArrayRef<Type> returnTypes;
  if (!isVoidType(returnType)) {
    returnTypes = llvm::makeArrayRef(returnType);
  }
  typeMap[operands[0]] = FunctionType::get(argTypes, returnTypes, context);
  return success();
}

//===----------------------------------------------------------------------===//
// Instruction
//===----------------------------------------------------------------------===//

LogicalResult Deserializer::sliceInstruction(spirv::Opcode &opcode,
                                             ArrayRef<uint32_t> &operands) {
  auto binarySize = binary.size();
  if (curOffset >= binarySize) {
    return failure();
  }
  // For each instruction, get its word count from the first word to slice it
  // from the stream properly, and then dispatch to the instruction handler.

  uint32_t wordCount = binary[curOffset] >> 16;
  opcode = static_cast<spirv::Opcode>(binary[curOffset] & 0xffff);

  if (wordCount == 0)
    return emitError(unknownLoc, "word count cannot be zero");

  uint32_t nextOffset = curOffset + wordCount;
  if (nextOffset > binarySize)
    return emitError(unknownLoc, "insufficient words for the last instruction");

  operands = binary.slice(curOffset + 1, wordCount - 1);
  curOffset = nextOffset;
  return success();
}

LogicalResult Deserializer::processInstruction(spirv::Opcode opcode,
                                               ArrayRef<uint32_t> operands) {
  // First dispatch all the instructions whose opcode does not correspond to
  // those that have a direct mirror in the SPIR-V dialect
  switch (opcode) {
  case spirv::Opcode::OpMemoryModel:
    return processMemoryModel(operands);
  case spirv::Opcode::OpTypeFloat:
  case spirv::Opcode::OpTypeFunction:
  case spirv::Opcode::OpTypePointer:
  case spirv::Opcode::OpTypeVoid:
    return processType(opcode, operands);
  case spirv::Opcode::OpFunction:
    return processFunction(operands);
  default:;
  }
  return dispatchToAutogenDeserialization(opcode, operands);
}

namespace {
// Pull in auto-generated Deserializer::dispatchToAutogenDeserialization() and
// various processOpImpl specializations.
#define GET_DESERIALIZATION_FNS
#include "mlir/Dialect/SPIRV/SPIRVSerialization.inc"
} // namespace

Optional<spirv::ModuleOp> spirv::deserialize(ArrayRef<uint32_t> binary,
                                             MLIRContext *context) {
  Deserializer deserializer(binary, context);

  if (failed(deserializer.deserialize()))
    return llvm::None;

  return deserializer.collect();
}
