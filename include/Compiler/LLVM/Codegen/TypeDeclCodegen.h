#ifndef TYPEDECLCODEGEN_H
#define TYPEDECLCODEGEN_H

#pragma once

namespace AST
{
    class TypeDeclNode;
    class MemberAccessNode;
    class VarNode;
    class NodeReference;
};

namespace llvm
{
    class Value;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // lowers struct declarations and struct member access/mutation, including chained member
    // access and pointer dereferencing to reach the addressed field
    class TypeDeclCodegen
    {
    public:
        TypeDeclCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        void gen_type_decl(AST::TypeDeclNode &node);
        void gen_member_access(AST::MemberAccessNode &node);
        void gen_var(AST::VarNode &node);

    private:
        CodegenContext &_ctx;
    };
};

#endif
