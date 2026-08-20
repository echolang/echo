#ifndef ERASURECODEGEN_H
#define ERASURECODEGEN_H

#pragma once

namespace llvm
{
    class Value;
};

namespace AST
{
    class FunctionCallExprNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // owning class-handle erasure: `erased::from`, its retain/release, and `assume<T>`.
    //
    // a subsystem rather than an arm on ExprCodegen because the four builtins share one layout -
    // `$object` and `$release` found by name on the `#[core: erased]` type - and because the count
    // work is ClassCodegen's. ExprCodegen dispatches; this file seats the struct and asks the class
    // subsystem to move the count. missing properties throw rather than assuming slot 0
    class ErasureCodegen
    {
    public:
        ErasureCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        void gen_from(AST::FunctionCallExprNode &node);
        void gen_retain(AST::FunctionCallExprNode &node);
        void gen_release(AST::FunctionCallExprNode &node);
        void gen_assume(AST::FunctionCallExprNode &node);

    private:
        CodegenContext &_ctx;

        unsigned object_index();
        unsigned release_index();
        unsigned require_property(const char *name);

        // argument 0 as the `erased` aggregate: a borrow loads, a value is already it
        llvm::Value *load_erased(AST::FunctionCallExprNode &node);
    };
};

#endif
