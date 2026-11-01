#ifndef TYPELAYOUT_H
#define TYPELAYOUT_H

#pragma once

#include "AST/ASTLayoutQuery.h"
#include "Compiler/CompilerOptions.h"

#include <memory>
#include <optional>

namespace Compiler::LLVM
{
    // **the DataLayout answer, installed for a FrontEnd's life.** const_fold asks
    // AST::layout_alloc_size; codegen's size_of asks the same functions, so a `const if`
    // and a runtime `mem::size<T>()` cannot disagree
    class LayoutQueryScope
    {
    public:
        explicit LayoutQueryScope(const Compiler::CompilerOptions &options);
        ~LayoutQueryScope();
        LayoutQueryScope(const LayoutQueryScope &) = delete;
        LayoutQueryScope &operator=(const LayoutQueryScope &) = delete;
        LayoutQueryScope(LayoutQueryScope &&) = delete;
        LayoutQueryScope &operator=(LayoutQueryScope &&) = delete;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
        std::optional<AST::LayoutQueryScope> _ast;
    };
};

#endif
