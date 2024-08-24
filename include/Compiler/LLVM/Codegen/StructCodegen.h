#ifndef STRUCTCODEGEN_H
#define STRUCTCODEGEN_H

#pragma once

namespace AST
{
    class StructDeclNode;
    class MemberAccessNode;
    class VarNode;
    class MemberMutNode;
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
    // access and pointer dereferencing to reach the addressed field.
    class StructCodegen
    {
    public:
        StructCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        void gen_struct_decl(AST::StructDeclNode &node);
        void gen_member_access(AST::MemberAccessNode &node);
        void gen_var(AST::VarNode &node);
        void gen_member_mut(AST::MemberMutNode &node);

    private:
        CodegenContext &_ctx;

        // returns a pointer to the struct value that `base` evaluates to. a VarRef yields the
        // variable's storage (dereferenced once when the variable is itself a pointer); a
        // MemberAccessNode recurses through gen_member_ptr. this walks chains of any depth
        llvm::Value *gen_struct_ptr(const AST::NodeReference &base);

        // returns a pointer to the field addressed by `node` (its address, not the loaded value)
        llvm::Value *gen_member_ptr(AST::MemberAccessNode &node);
    };
};

#endif
