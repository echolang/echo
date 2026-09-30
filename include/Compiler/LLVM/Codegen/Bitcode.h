#ifndef BITCODE_H
#define BITCODE_H

#pragma once

#include <string>

namespace llvm
{
    class Module;
};

namespace Compiler::LLVM
{
    // one bitcode writer. partition materialize and isolated emit both round-trip IR
    std::string bitcode_of(llvm::Module &module);
};

#endif
