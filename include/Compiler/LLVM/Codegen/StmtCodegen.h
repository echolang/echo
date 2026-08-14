#ifndef STMTCODEGEN_H
#define STMTCODEGEN_H

#pragma once

namespace llvm
{
    class AllocaInst;
    class Type;
    class Value;
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
    class ForStatementNode;
    class LoopControlNode;
    class AssignNode;
    class ExprNode;
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

        // **write an aggregate into storage one leaf field at a time, never as a whole-struct store.**
        // the granularity is the point: a `store %Foo %v, ptr %slot` of an already-assembled value is
        // something SROA folds back into the insertvalue chain it came from, restoring the first-class
        // aggregate this ABI exists to remove. recursive, since a nested aggregate has the same problem
        // one level in - an enum whose payload is an enum lowers to `{ i8, { i8 } }`
        void store_aggregate_fieldwise(llvm::Value *value, llvm::Value *slot, llvm::Type *type);
        void gen_if_statement(AST::IfStatementNode &node);

        // `guard T $x = <nullable> else { ... }`. evaluate the nullable **once**, test it, and either
        // store the unwrapped value into `$x`'s slot and carry on, or run the else arm - which
        // AST::scope_always_exits has already proven does not come back
        //
        // so there is no merge block and no phi: the two paths never rejoin, which is exactly what makes
        // `$x` a plain non-null local from here on rather than something every later read has to re-check
        void gen_guard(AST::GuardNode &node);
        void gen_while_statement(AST::WhileStatementNode &node);

        // `for (init; condition; step)`. the init is not here - it is the preceding statement in the
        // wrapper scope Parser::parse_forstatement minted, so codegen has already emitted it
        void gen_for_statement(AST::ForStatementNode &node);

        // `break` / `continue`: the exit's own drops, then a branch to whichever of the innermost
        // CodegenContext::LoopTarget's two blocks the kind names
        void gen_loop_control(AST::LoopControlNode &node);
        void gen_assign(AST::AssignNode &node);

    private:

        // **the one loop lowering**, which both statements above go through. a `while` passes no step and
        // its `continue` target is the condition block; a `for` passes one and its `continue` target is
        // the step block. that is the entire difference between the two, said once
        void gen_loop(AST::ExprNode &condition, AST::ScopeNode *step, AST::ScopeNode &body);

        CodegenContext &_ctx;
    };
};

#endif
