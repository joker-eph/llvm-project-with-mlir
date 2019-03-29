//===- FileUtilities.cpp - utilities for working with files -----*- C++ -*-===//
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
// Definitions of common utilities for working with files.
//
//===----------------------------------------------------------------------===//

#include "mlir/Support/FileUtilities.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/ToolOutputFile.h"

using namespace mlir;

std::unique_ptr<llvm::ToolOutputFile>
mlir::openOutputFile(StringRef outputFilename) {
  std::error_code error;
  auto result = llvm::make_unique<llvm::ToolOutputFile>(outputFilename, error,
                                                        llvm::sys::fs::F_None);
  if (error) {
    llvm::errs() << error.message();
    return nullptr;
  }

  return result;
}
