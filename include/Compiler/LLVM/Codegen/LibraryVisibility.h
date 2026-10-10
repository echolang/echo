#ifndef LIBRARYVISIBILITY_H
#define LIBRARYVISIBILITY_H

#pragma once

namespace Compiler::LLVM
{
    struct CodegenContext;

    // a native library's defined symbols are hidden except `#[export]`.
    // export names are collected from every unit's function table, so a
    // whole-program merge of a dependency's `#[export]` stays visible the
    // same way the per-unit path keeps it. wasm and executables never call this
    void hide_non_exported_symbols(CodegenContext &ctx);
};

#endif
