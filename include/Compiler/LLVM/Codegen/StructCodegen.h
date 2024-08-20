#ifndef STRUCTCODEGEN_H
#define STRUCTCODEGEN_H

#pragma once

namespace AST
{
    class StructDeclNode;
    class MemberAccessNode;
    class VarNode;
    class VarMemberNode;
    class MemberMutNode;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // lowers struct declarations and struct member access/mutation, including chained member
    // access and pointer dereferencing to reach the addressed field.
    class StructCodegen
    {
    public:
        StructCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        void gen_struct_decl(AST::StructDeclNode &node);
        void gen_member_access(AST::MemberAccessNode &node);
        void gen_var(AST::VarNode &node);
        void gen_var_member(AST::VarMemberNode &node);
        void gen_member_mut(AST::MemberMutNode &node);

    private:
        CodegenContext &_ctx;
    };
}

#endif
