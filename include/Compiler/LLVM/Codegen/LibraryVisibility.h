#ifndef LIBRARYVISIBILITY_H
#define LIBRARYVISIBILITY_H

#pragma once

namespace Compiler::LLVM
{
    struct CmpUnit;

    // a native library's defined symbols are hidden except `#[export]`.
    // keyed on FunctionDeclNode::export_name through the unit's function table,
    // so keep-visible is the language fact rather than a storage class Unix ignores.
    // wasm and executables never call this
    void hide_non_exported_symbols(CmpUnit &unit);
};

#endif
