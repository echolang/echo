#ifndef CLASSCODEGEN_H
#define CLASSCODEGEN_H

#pragma once

#include "Compiler/LLVM/Codegen/ClassLayout.h"
#include "Compiler/LLVM/Codegen/CountAtomics.h"

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>

namespace AST
{
    class ClassAllocExprNode;
    class InstanceOfExprNode;
    class ComplexType;
    class ReleaseNode;
    class RetainExprNode;
    class ValueType;
};

namespace Compiler::LLVM
{
    struct CodegenContext;
    struct CmpUnit;

    // everything that is true of a class and of nothing else: allocating the heap block, moving the
    // strong count, and tearing the block down when the count reaches zero
    //
    // it is all here rather than spread over the expression and statement subsystems because the three
    // share one invariant - the block layout in Codegen/ClassLayout.h - and because retain and release
    // have to agree exactly about which word they are moving. the *policy* (which reads retain, which
    // scopes release) is not here at all: AST::OwnershipPass decides that and writes it into the tree,
    // so this file only ever answers "emit one retain here"
    class ClassCodegen
    {
    public:
        ClassCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        // a fresh block: malloc, zero it whole, seat both counts at 1, write the typeinfo
        // pushes the handle
        void gen_class_alloc(AST::ClassAllocExprNode &node);

        // the two nodes AST::OwnershipPass writes into the tree
        void gen_retain_expr(AST::RetainExprNode &node);
        void gen_release_stmt(AST::ReleaseNode &node);

        // `E instanceof T`: dispatches on what `T` is, because the two questions have different shapes.
        // against a **class** it is one address comparison of the block's identity word against T's
        // identity global - there is no inheritance for a subtype check to walk. against an
        // **interface** it is a scan of the block's conformance table, since conformance is a set and
        // not an identity. against a struct it folds to false, which needs no runtime at all
        void gen_instanceof(AST::InstanceOfExprNode &node);


        // "move the strong count of this value", whatever kind of value it is. the one entry point for
        // both, so a teardown site added later cannot forget that a callable counts its environment
        // rather than a class block - which would be the wrong thunk with no diagnostic
        //
        // the retain hands the value back unchanged, so it can sit inline in an expression
        llvm::Value *gen_retain_value(llvm::Value *value, const AST::ValueType &type);
        void gen_release_value(llvm::Value *value, const AST::ValueType &type);

        // the heap block for a class value: malloc, zero, strong count 1, typeinfo. shared by
        // gen_class_alloc and by a closure's environment, which is a class the compiler declared rather
        // than one the user did - so the two cannot end up with differently shaped blocks
        llvm::Value *gen_class_box_alloc(const AST::ValueType &class_type);

        // the weak half of the count, the two operations `weak<T>` is made of. neither needs a deinit or
        // a layout beyond the shared header, so neither is per class the way the strong release is
        //
        // gen_weak_of turns a live handle into a weak one: null stays null, anything else takes a weak
        // reference and is handed back. `&$obj` and `weak($obj)` are both this
        llvm::Value *gen_weak_of(llvm::Value *handle, const AST::ValueType &class_type);

        // and the upgrade back, which is the only thing that can *fail*: null stays null, a block whose
        // strong count already reached zero answers null - the payload is gone, and the block is only
        // still readable because this weak reference was holding it - and anything else takes a strong
        // reference and is handed back. that three-way answer is why `strong($w)` is typed `T?`
        llvm::Value *gen_strong_upgrade(llvm::Value *weak_handle, const AST::ValueType &class_type);

        // the one release implementation per class per compilation unit, created on first use
        //
        //   void __eco_release_<mangled>(ptr handle)
        //
        // null-check, decrement, return unless zero, call the class's deinit if it has one, then drop the
        // one weak reference the strong ones collectively held - which is what frees the block, in
        // __eco_weak_release rather than here. see Codegen/ClassLayout.h for why there is exactly one free.
        // the deinit is an ordinary Echo function the ownership pass synthesized out of the same
        // emit_drop recursion a struct's scope exit uses - so what a class destroys at zero and what a
        // struct destroys at scope end are decided in exactly one place
        //
        // **public**, because an interface vtable holds it in slot 0: an erased value owns a reference and
        // the release site knows only the interface, so the thunk has to be reachable from the value. see
        // Codegen/IfaceValue.h. that caller wants the function as a *constant*, not a call to it, which
        // is why it takes this rather than gen_release_value below
        llvm::Function *get_or_create_release_thunk(const AST::ValueType &class_type);

        // one of `handle`'s counts as an i64, or **0 when it is null**. here rather than at its callers so
        // `ClassBox::strong_index` and `weak_index` keep one owner.
        //
        // reached from Echo through the `ref_count` and `weak_count` builtins. the strong one's one real
        // consumer is a copy-on-write container asking "am I the only owner"; the weak one exists so the
        // corpus can pin what a weak reference does to the block without a leak checker. see the
        // implementation for why null answers zero
        llvm::Value *gen_count(
            llvm::Value *handle,
            const AST::ValueType &class_type,
            unsigned index
        );

    private:
        CodegenContext &_ctx;

        // the two arms gen_retain_value / gen_release_value dispatch to. a class moves the count in its
        // block; a callable moves the one in its *environment*, which is the only thing a callable owns
        //
        // the callable arm is uniform rather than per type, and that is forced: a callable's static type
        // is its signature and says nothing about which environment it holds, so the teardown cannot be
        // keyed on a class the way the class arm is. the env is a minted atomic class; last release
        // loads the typeinfo deinit, which is why an owning capture needs no arm here
        llvm::Value *gen_retain(llvm::Value *handle, const AST::ValueType &class_type);
        void gen_release(llvm::Value *handle, const AST::ValueType &class_type);

        llvm::Value *gen_callable_retain(llvm::Value *callable);
        void gen_callable_release(llvm::Value *callable);

        // the interface arms. an erased value counts the *object* it holds - the count is in that block,
        // where a class handle's is - so the retain is the ordinary one over field 0.
        //
        // the release is the interesting half: which thunk to call is not knowable from the static type,
        // so it is read out of the value's own vtable at the reserved release slot. that is the same trade
        // the whole fat pointer makes - resolve at the widening what would otherwise be searched for -
        // and it is why an erased release is one indirect call rather than a conformance scan
        llvm::Value *gen_iface_retain(llvm::Value *erased);
        void gen_iface_release(llvm::Value *erased);

        // += 1 on one of `block`'s counts, guarded on `block` being non-null - a class handle is nullable
        // and a retain of null is how `Foo $a = $b;` behaves when `$b` holds nothing.
        //
        // a null `layout` reaches the word through the shared header instead, which is how an environment
        // and an erased operand are counted: the one thing neither knows is a class layout
        void gen_count_inc(
            llvm::Value *block,
            const ClassLayout *layout,
            unsigned index,
            const char *label,
            CountAccess access);

        // the interface half of gen_instanceof: walk the block's conformance table looking for the
        // interface's identity global. a loop rather than a comparison because conformance is a *set* -
        // a class may answer several interfaces, and which slot one sits in is not knowable from the
        // interface alone. pushes an i1
        //
        // `handle` is already evaluated and already known non-null: the null guard is the caller's, since
        // it is the same guard the class arm needs and a second one would be a second answer to "is null
        // an instance of anything"
        //
        // `box_type` rather than a ClassLayout, because the box is the only field of one this needs and
        // an erased operand has no layout to fill the rest of - handing over a default-constructed
        // ClassLayout with one field set makes a half-valid value the callee has to be trusted not to read
        llvm::Value *gen_conformance_scan(
            llvm::Value *handle,
            llvm::Type *box_type,
            const AST::ComplexType &interface
        );

        // the environment counterpart, one per compilation unit rather than one per type:
        //
        //   void __eco_release_env(ptr handle)
        //
        // teardown is dynamic: the typeinfo deinit slot is loaded and called when the count
        // hits zero. null when the environment owns nothing
        llvm::Function *get_or_create_env_release_thunk();

        // and the weak one, also one per compilation unit and for a stronger reason: a weak release runs
        // no deinit and reads no property, so there is nothing per class for it to know
        //
        //   void __eco_weak_release(ptr handle)
        //
        // null-check, decrement __weak, `free` when it reaches zero. **the only free in the runtime** -
        // every teardown path, strong and weak, ends here, which is what makes it impossible for two
        // counts to disagree about whose job the free was
        llvm::Function *get_or_create_weak_release_thunk();

        // the body both of the above are: linkonce_odr `void <name>(ptr handle)`, null-check, decrement,
        // return unless zero, `complex`'s deinit if there is one, free. built with its own builder
        // position, which is saved and restored - this is called from the middle of whatever function
        // asked for a release
        //
        // `layout` and `complex` are both null for an environment, which has neither
        llvm::Function *build_release_thunk(
            const std::string &name,
            const ClassLayout *layout,
            const AST::ComplexType *complex
        );

        // `complex`'s deinit, called on a handle spilled to a slot so it can be addressed the way every
        // other receiver is. does nothing when the type declares none, which is the common case
        void gen_deinit_call(const AST::ComplexType *complex, llvm::Value *handle);

        // the environment half: load the typeinfo deinit slot, skip if null, otherwise the same
        // spill-and-call. one path for every environment, because a callable's type says nothing
        // about which environment it holds
        void gen_typeinfo_deinit_call(llvm::Value *handle);

        // **one decrement, two things that can end at zero.** the strong count reaching zero runs the
        // payload's teardown and gives back the collective weak reference; the weak count reaching zero
        // frees the block. everything around that is identical - the linkonce_odr `void(ptr)` thunk, the
        // saved and restored builder position, the null check, the load/sub/store, and the branch on zero -
        // and it was written out twice before this, which is two decrement sequences that could disagree
        // about which word they were touching
        //
        // `on_zero` is emitted into the block reached at zero, named `zero_block_name`; the branch back to
        // `done` and the return are added around whatever it emits. the IR labels for the count itself come
        // from `count_name(count_index)`, so they cannot name a different word than the GEP does
        // declaring the symbol is split from emitting the body because *when* the symbol joins the module
        // is observable: it fixes the order the definitions are printed in, and a strong release has to be
        // declared before it asks for the weak thunk it calls, so the call reads above the definition
        llvm::Function *declare_release_thunk(const std::string &name, const char *handle_name);

        llvm::Function *build_count_release_thunk(
            llvm::Function *thunk,
            llvm::Type *box_type,
            unsigned count_index,
            const char *zero_block_name,
            llvm::function_ref<void(llvm::Value *handle)> on_zero,
            CountAccess access
        );

        // the address of one header word inside `handle`'s block. takes the box type rather than a layout,
        // because half its callers do not have one: an erased operand and a closure environment know only
        // the shared header, and `TypeLowering::class_header_llvm_type()` is a prefix of every box by
        // construction - the payload is wrapped, never prefixed
        //
        // it exists because the strong count is not at offset 0 once __weak joins it.
        // letting an environment treat the handle *as* the count's address would silently
        // decrement the wrong word
        llvm::Value *gen_header_ptr(
            llvm::Value *handle,
            llvm::Type *box_type,
            unsigned index,
            const llvm::Twine &name
        );

        // the box type to GEP through: the layout's when there is one, the shared header otherwise
        llvm::Type *header_box_type(const ClassLayout *layout);
    };
};

#endif
