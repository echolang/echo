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
    class Instruction;
    class StoreInst;
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
    // **where an address came from, which decides whether an access through it may be tagged.**
    //
    // a *typed* place was reached without ever leaving the compiler's own accounting: a local, a
    // field of one, an element a container handed back. its storage is the type it says it is,
    // because the only way to make that false is a reinterpretation, and that now needs `unsafe`.
    //
    // a *raw* place went through a `ptr<T>` - the pointer that a reinterpretation produces, the one
    // `mem::copy` walks bytes through, the one an FFI call hands back. an access through it is
    // emitted with no `!tbaa` at all, and an untagged instruction may alias anything: the
    // conservative answer, said by omission
    enum class Provenance
    {
        t_typed,
        t_raw,
    };

    struct LValue
    {
        llvm::Value *address = nullptr;

        // st(E) in the model: the type of the thing *at* `address`, before any auto-deref
        // for `ptr<int32> $p` this is ptr<int32>, and the address is $p's own slot
        AST::ValueType storage_type;

        // **it travels with the address for storage_type's reason.** by the time a load is emitted
        // the expression is long gone, and "was there a raw pointer anywhere on the way here" is not
        // a question an `llvm::Value *` can be asked
        Provenance provenance = Provenance::t_typed;
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

        // **the store half of the seam, and the mirror of gen_load above.** it existed only as
        // fifteen scattered `CreateStore` calls, which was survivable while a store was just a
        // store - and stopped being once an access had to carry metadata, because a fact attached
        // at one of fifteen sites is a fact missing from fourteen
        llvm::StoreInst *gen_store(const LValue &place, llvm::Value *value);

        // gen_lvalue followed by gen_load: the ordinary "read this expression" path
        llvm::Value *gen_load(AST::ExprNode &expr, const char *name);

        // gen_lvalue with one auto-deref applied when the storage holds a pointer, so the
        // result addresses the pointee. that is what a plain read or write of a transparent
        // pointer wants: `$p = 20` stores into the pointee, never into the slot
        //
        // two callers, and between them the whole of the rule: gen_lvalue's own deref arm - so this
        // *is* what a DerefExprNode lowers to, which is the collapse this comment used to predict
        // rather than the retirement it expected - and a member access's base, which
        // AST::PointerAdjuster deliberately leaves un-derefed because `->` reaches through every
        // level where that pass only ever inserts one (its n_member_access arm says so)
        //
        // also the one place that answers for an expression with *no* storage of its own: the value
        // of a pointer-typed non-place already is the address. see the body
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

        // attaches the type tag, or deliberately nothing. one place, so a load and a store cannot
        // come to disagree about what an access at a type may reach
        void tag_access(llvm::Instruction *access, const LValue &place);
    };
};

#endif
