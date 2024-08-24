#ifndef STMTCODEGEN_H
#define STMTCODEGEN_H

#pragma once

namespace AST
{
    class ScopeNode;
    class VarDeclNode;
    class FunctionDeclNode;
    class ReturnNode;
    class IfStatementNode;
    class WhileStatementNode;
    class VarMutNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // lowers statements and control flow: scopes, variable declarations & mutations, returns,
    // if/while blocks, and the bodies of concrete function declarations.
    class StmtCodegen
    {
    public:
        StmtCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        void gen_scope(AST::ScopeNode &node);
        void gen_var_decl(AST::VarDeclNode &node);
        void gen_function_decl(AST::FunctionDeclNode &node);
        void gen_return(AST::ReturnNode &node);
        void gen_if_statement(AST::IfStatementNode &node);
        void gen_while_statement(AST::WhileStatementNode &node);
        void gen_var_mut(AST::VarMutNode &node);

    private:
        CodegenContext &_ctx;
    };
};

#endif
