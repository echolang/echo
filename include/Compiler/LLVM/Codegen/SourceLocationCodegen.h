#ifndef SOURCELOCATIONCODEGEN_H
#define SOURCELOCATIONCODEGEN_H

#pragma once

#include "AST/ASTBuiltin.h"
#include "AST/ASTSourceToken.h"

#include <llvm/IR/Value.h>

#include <string>

namespace AST
{
    class FunctionCallExprNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // the one owner of a `std::src::location` as a value: interned here()/caller(), and
    // the `$at` / `$__caller` choice a stop reads
    class SourceLocationCodegen
    {
    public:
        SourceLocationCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        void gen_builtin(AST::FunctionCallExprNode &node, AST::BuiltinKind kind);

        void abort_from_call(
            AST::FunctionCallExprNode &node,
            const std::string &headline,
            llvm::Value *detail_ptr,
            llvm::Value *detail_len
        );

    private:
        CodegenContext &_ctx;

        llvm::Value *intern(const AST::SourceSite &site, const std::string &function);
        llvm::Value *caller_address();
    };
};

#endif
