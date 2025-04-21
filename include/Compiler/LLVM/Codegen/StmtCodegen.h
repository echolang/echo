#ifndef STMTCODEGEN_H
#define STMTCODEGEN_H

#pragma once

namespace llvm
{
    class AllocaInst;
};

namespace AST
{
    class ScopeNode;
    class VarDeclNode;
    class FunctionDeclNode;
    class ReturnNode;
    class IfStatementNode;
    class GuardNode;
    class WhileStatementNode;
    class LoopControlNode;
    class AssignNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // lowers statements and control flow: scopes, variable declarations & mutations, returns,
    // if/while blocks, and the bodies of concrete function declarations
    class StmtCodegen
    {
    public:
        StmtCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        void gen_scope(AST::ScopeNode &node);
        void gen_var_decl(AST::VarDeclNode &node);

        // the storage half of a declaration, idempotent: the slot and the zero-init a value needs before
        // anything can legitimately read it. gen_scope calls it for every declaration in a scope before
        // the first statement, so *where* a declaration sits among its siblings decides nothing
        llvm::AllocaInst *ensure_var_slot(AST::VarDeclNode &node);

        void gen_function_decl(AST::FunctionDeclNode &node);
        void gen_return(AST::ReturnNode &node);
        void gen_if_statement(AST::IfStatementNode &node);

        // `guard T $x = <nullable> else { ... }`. evaluate the nullable **once**, test it, and either
        // store the unwrapped value into `$x`'s slot and carry on, or run the else arm - which
        // AST::scope_always_exits has already proven does not come back
        //
        // so there is no merge block and no phi: the two paths never rejoin, which is exactly what makes
        // `$x` a plain non-null local from here on rather than something every later read has to re-check
        void gen_guard(AST::GuardNode &node);
        void gen_while_statement(AST::WhileStatementNode &node);

        // `break` / `continue`: the exit's own drops, then a branch to whichever of the innermost
        // CodegenContext::LoopTarget's two blocks the kind names
        void gen_loop_control(AST::LoopControlNode &node);
        void gen_assign(AST::AssignNode &node);

    private:
        CodegenContext &_ctx;
    };
};

#endif
