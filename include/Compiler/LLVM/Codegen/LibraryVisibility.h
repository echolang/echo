#ifndef LIBRARYVISIBILITY_H
#define LIBRARYVISIBILITY_H

#pragma once

namespace Compiler::LLVM
{
    struct CodegenContext;

    // a native library's defined symbols are hidden except those that carry
    // `DLLExport`, which TypeLowering plants on every native-lib `#[export]`.
    // the marker is the keep-visible rule so a later ABI table global with the
    // same storage class is kept without a name set. wasm and executables
    // never call this
    void hide_non_exported_symbols(CodegenContext &ctx);
};

#endif
