#ifndef LVALUECODEGEN_H
#define LVALUECODEGEN_H

#pragma once

#include "AST/ASTValueType.h"

namespace AST
{
    class ExprNode;
};

namespace llvm
{
    class Value;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // an addressable location: where the storage lives, and what it holds
    //
    // the type has to travel with the address. under llvm's opaque pointers every pointer is
    // the same `ptr`, so an llvm::Value alone says nothing about what it points at - and every
    // load needs its element type spelled out
    struct LValue
    {
        llvm::Value *address = nullptr;

        // st(E) in the model: the type of the thing *at* `address`, before any auto-deref
        // for `ptr<int32> $p` this is ptr<int32>, and the address is $p's own slot
        AST::ValueType storage_type;
    };

    // the one place that turns an expression into an address
    //
    // before this existed the only address path was private to TypeDeclCodegen and hardcoded to
    // a two case switch over variable and member bases, which is why `&$s->x` could not be
    // spelled and why member access and member mutation drifted apart (todo/A3, todo/B4)
    class LValueCodegen
    {
    public:
        LValueCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        // addr(E) and st(E). never dereferences: for a pointer variable this hands back the
        // variable's own slot, not the thing it points at. that is what `&E` and `E:$` want
        LValue gen_lvalue(AST::ExprNode &expr);

        // reads the value out of a place. every read in the compiler goes through here, so the
        // "which llvm type do I load" question - the one opaque pointers make unanswerable from
        // the address alone - is answered from storage_type in exactly one place
        llvm::Value *gen_load(const LValue &place, const char *name);

        // gen_lvalue followed by gen_load: the ordinary "read this expression" path
        llvm::Value *gen_load(AST::ExprNode &expr, const char *name);

        // gen_lvalue with one auto-deref applied when the storage holds a pointer, so the
        // result addresses the pointee. that is what a plain read or write of a transparent
        // pointer wants: `$p = 20` stores into the pointee, never into the slot
        //
        // deliberately temporary. once an explicit deref node exists, every value position
        // carries its own deref in the tree and this collapses into
        // gen_lvalue(Deref(E)) - do not build anything new on it
        LValue gen_place(AST::ExprNode &expr);

        // evaluates `expr` as a pointer-typed rvalue, i.e. the address it holds rather than
        // the address it lives at. the seam pointer indexing and arithmetic need
        llvm::Value *gen_address_value(AST::ExprNode &expr);

    private:
        CodegenContext &_ctx;

        // one level of transparency: loads the address out of a pointer-holding place, so the
        // result addresses the pointee. returns `place` untouched when it holds no pointer
        LValue deref_once(const LValue &place);

        // the address of the struct that a member access is reaching into, with any pointer
        // base already dereferenced
        LValue gen_member_lvalue(AST::ExprNode &expr);
    };
};

#endif
