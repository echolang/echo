#ifndef ASTLAYOUTQUERY_H
#define ASTLAYOUTQUERY_H

#pragma once

#include "AST/ASTValueType.h"

#include <optional>

namespace AST
{
    // **the target's alloc size and ABI align of a ValueType**, available before codegen so a
    // `const if` can branch on `mem::size<T>()`. Compiler::LLVM::LayoutQueryScope installs
    // the DataLayout answer. without one, size_of stays a located refusal
    std::optional<uint64_t> layout_alloc_size(const ValueType &type);
    std::optional<uint64_t> layout_abi_align(const ValueType &type);

    typedef std::optional<uint64_t> (*LayoutQueryFn)(const ValueType &type);

    class LayoutQueryScope
    {
    public:
        LayoutQueryScope(LayoutQueryFn alloc_size, LayoutQueryFn abi_align);
        ~LayoutQueryScope();
        LayoutQueryScope(const LayoutQueryScope &) = delete;
        LayoutQueryScope &operator=(const LayoutQueryScope &) = delete;
        LayoutQueryScope(LayoutQueryScope &&) = delete;
        LayoutQueryScope &operator=(LayoutQueryScope &&) = delete;

    private:
        LayoutQueryFn _previous_size;
        LayoutQueryFn _previous_align;
    };
};

#endif
