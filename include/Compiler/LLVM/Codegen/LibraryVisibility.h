#ifndef LIBRARYVISIBILITY_H
#define LIBRARYVISIBILITY_H

#pragma once

namespace llvm
{
    class Module;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // hide defined symbols except those with DLLExport
    void hide_non_exported_symbols(CodegenContext &ctx);
    void hide_non_exported_symbols(llvm::Module &module);
};

#endif
