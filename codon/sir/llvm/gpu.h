#pragma once

#include <string>

#include "codon/sir/llvm/llvm.h"

namespace codon {
namespace ir {

/// Applies GPU-specific transformations and generates PTX
/// code from kernel functions in the given LLVM module.
/// @param module LLVM module containing GPU kernel functions (marked with "kernel"
/// annotation)
/// @param ptxFilename Filename for output PTX code; empty to use filename based on
/// module
void applyGPUTransformations(llvm::Module *module, const std::string &ptxFilename = "");

} // namespace ir
} // namespace codon
