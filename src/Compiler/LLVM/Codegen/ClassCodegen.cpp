#include "Compiler/LLVM/Codegen/ClassCodegen.h"
#include "Compiler/LLVM/Codegen/IfaceValue.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/Codegen/LValueCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"

#include "AST/ASTMangler.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ReleaseNode.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <fmt/core.h>

namespace Compiler::LLVM
{

llvm::FunctionCallee ClassCodegen::get_malloc()
{
    return _ctx.libc_callee("malloc",
        _ctx.opaque_ptr_type(),
        { llvm::Type::getInt64Ty(*_ctx.llvm_context) });
}

llvm::FunctionCallee ClassCodegen::get_free()
{
    return _ctx.libc_callee("free",
        llvm::Type::getVoidTy(*_ctx.llvm_context),
        { _ctx.opaque_ptr_type() });
}

llvm::Value *ClassCodegen::gen_strong_ptr(llvm::Value *handle, const ClassLayout &layout)
{
    return _ctx.builder->CreateStructGEP(layout.box, handle, ClassBox::strong_index, "strong_ptr");
}

llvm::Value *ClassCodegen::gen_typeinfo_ptr(llvm::Value *handle, llvm::Type *box_type)
{
    return _ctx.builder->CreateStructGEP(box_type, handle, ClassBox::typeinfo_index, "typeinfo_ptr");
}

void ClassCodegen::gen_class_alloc(AST::ClassAllocExprNode &node)
{
    _ctx.push(gen_class_box_alloc(node.class_type));
}

llvm::Value *ClassCodegen::gen_class_box_alloc(const AST::ValueType &class_type)
{
    const ClassLayout layout =
        _ctx.types->get_or_create_class_layout(class_type.get_complex_type(), *_ctx.current_cmp_unit);

    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    const uint64_t block_size = _ctx.layout().getTypeAllocSize(layout.box);

    llvm::Value *handle = _ctx.builder->CreateCall(
        get_malloc(), { llvm::ConstantInt::get(i64, block_size) }, "obj");

    // the whole block, header included, before either header word is written. a constructor writes
    // only the properties it was given, and a property nobody writes is read by the very first thing
    // that touches it: the release of an overwritten class-typed field, or a destructor over a
    // pointer field. so this is a correctness prerequisite rather than tidiness, and it is the same
    // reasoning behind zero-filling a struct alloca with no initializer
    _ctx.builder->CreateMemSet(
        handle,
        llvm::ConstantInt::get(llvm::Type::getInt8Ty(*_ctx.llvm_context), 0),
        llvm::ConstantInt::get(i64, block_size),
        llvm::MaybeAlign(_ctx.layout().getABITypeAlign(layout.box)));

    // one strong reference: the value this expression hands back. it is the constructor's `$this`, a
    // body-local, and the implicit `return $this` moves it out - so the +1 travels to the caller
    // without a retain anywhere
    _ctx.builder->CreateStore(
        llvm::ConstantInt::get(i64, 1), gen_strong_ptr(handle, layout));

    _ctx.builder->CreateStore(
        layout.typeinfo,
        _ctx.builder->CreateStructGEP(layout.box, handle, ClassBox::typeinfo_index, "typeinfo_ptr"));

    return handle;
}

llvm::Value *ClassCodegen::gen_callable_retain(llvm::Value *callable)
{
    // the strong count is the block's first word, so the environment pointer *is* the count's address.
    // taken directly rather than through a ClassLayout GEP, because the layout is exactly what a
    // callable does not know
    gen_strong_inc(_ctx.builder->CreateExtractValue(callable, 1, "retain.env"), nullptr, "env.retain");

    // the value flows through: a retain is a side effect on the environment, not a new callable
    return callable;
}

void ClassCodegen::gen_callable_release(llvm::Value *callable)
{
    llvm::Value *env = _ctx.builder->CreateExtractValue(callable, 1, "release.env");
    _ctx.builder->CreateCall(get_or_create_env_release_thunk(), { env });
}

llvm::Function *ClassCodegen::get_or_create_env_release_thunk()
{
    const std::string name = "__eco_release_env";

    if (llvm::Function *existing = _ctx.current_module()->getFunction(name)) {
        return existing;
    }

    // no layout and no deinit: the count is the block's first word, and an environment holds no owning
    // capture, so the block is all there is to give back
    return build_release_thunk(name, nullptr, nullptr);
}

void ClassCodegen::gen_retain_expr(AST::RetainExprNode &node)
{
    node.operand->accept(*_ctx.visitor);

    llvm::Value *handle = _ctx.pop();

    // the handle flows straight through - the retain is a side effect on the block, so the value this
    // expression hands its consumer is the one the operand produced
    _ctx.push(gen_retain_value(handle, node.result_type()));
}

void ClassCodegen::gen_release_stmt(AST::ReleaseNode &node)
{
    // read the slot *now* rather than trusting a handle captured earlier: between the declaration and
    // this release an assignment may have re-seated the variable, and it is the current reference the
    // scope owes
    LValue place = _ctx.lvalues->gen_lvalue(*node.target);

    gen_release_value(_ctx.lvalues->gen_load(place, "obj"), place.storage_type);
}

llvm::Value *ClassCodegen::gen_retain_value(llvm::Value *value, const AST::ValueType &type)
{
    if (type.is_callable()) {
        return gen_callable_retain(value);
    }

    if (type.is_interface()) {
        return gen_iface_retain(value);
    }

    return gen_retain(value, type);
}

void ClassCodegen::gen_release_value(llvm::Value *value, const AST::ValueType &type)
{
    if (type.is_callable()) {
        gen_callable_release(value);
        return;
    }

    if (type.is_interface()) {
        gen_iface_release(value);
        return;
    }

    gen_release(value, type);
}

llvm::Value *ClassCodegen::gen_iface_retain(llvm::Value *erased)
{
    // an erased value is `{ object, vtable }` and only the object is counted. the *count* lives in the
    // object's own block, exactly where a class handle's does, so this needs no layout: gen_strong_inc
    // over a null layout reads the count as the block's first word - which is what the box puts there
    llvm::Value *object = _ctx.builder->CreateExtractValue(erased, { IfaceValue::object_index }, "iface.obj");
    gen_strong_inc(object, nullptr, "iface");

    // handed back unchanged so a retain can sit inline in an expression, as the other two do
    return erased;
}

void ClassCodegen::gen_iface_release(llvm::Value *erased)
{
    llvm::Type *opaque_ptr = llvm::PointerType::get(*_ctx.llvm_context, 0);

    // **which thunk** is the whole problem an erased release has: the static type is the interface, and
    // that says nothing about which block to free or which deinit to run. so it is read out of the value,
    // from the slot the widening filled - see Codegen/IfaceValue.h. one indirect call, no scan
    llvm::Value *object = _ctx.builder->CreateExtractValue(erased, { IfaceValue::object_index }, "iface.obj");
    llvm::Value *vtable = _ctx.builder->CreateExtractValue(erased, { IfaceValue::vtable_index }, "iface.vt");

    llvm::Value *release = _ctx.builder->CreateLoad(
        opaque_ptr,
        _ctx.builder->CreateConstGEP1_64(
            opaque_ptr, vtable, IfaceValue::vtable_release_slot, "iface.release_slot"),
        "iface.release");

    auto *thunk_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(*_ctx.llvm_context), { opaque_ptr }, false);

    // the thunk null-checks its own handle, exactly as a direct release does, so a null erased value needs
    // no guard here either
    _ctx.builder->CreateCall(thunk_type, release, { object });
}

void ClassCodegen::gen_instanceof(AST::InstanceOfExprNode &node)
{
    llvm::Type *i1 = llvm::Type::getInt1Ty(*_ctx.llvm_context);

    // a class operand tested against a struct. the answer is knowable here and it is always no: a
    // struct has no block, so nothing could ever have written its identity into one
    const bool against_interface = node.queried_type.is_interface();

    if (!node.queried_type.is_class() && !against_interface) {
        node.operand->accept(*_ctx.visitor);
        _ctx.pop();
        _ctx.push(llvm::ConstantInt::get(i1, 0));
        return;
    }

    // **an erased operand has no concrete layout**, so the typeinfo word is read through the header every
    // box shares. that header is identical by construction - the payload is wrapped, never prefixed - which
    // is the same fact gen_strong_inc leans on when it is handed a null layout
    const AST::ValueType operand_type = node.operand->result_type();
    const bool from_interface = operand_type.is_interface();

    llvm::Type *operand_box = from_interface
        ? _ctx.types->class_header_llvm_type()
        : _ctx.types->get_or_create_class_layout(
            operand_type.get_complex_type(), *_ctx.current_cmp_unit).box;

    node.operand->accept(*_ctx.visitor);
    llvm::Value *handle = _ctx.pop();

    // the object the erased value holds is what carries the identity; the vtable slot beside it says
    // nothing about which class is inside
    if (from_interface) {
        handle = _ctx.builder->CreateExtractValue(handle, { IfaceValue::object_index }, "iface.obj");
    }

    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    llvm::BasicBlock *entry_block = _ctx.builder->GetInsertBlock();
    llvm::BasicBlock *read_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "instanceof", function);
    llvm::BasicBlock *done_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "instanceof.done", function);

    // null is not an instance of anything, and there is no block to read the identity out of - so the
    // load has to be guarded rather than merely answered false afterwards
    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(handle), done_block, read_block);

    _ctx.builder->SetInsertPoint(read_block);

    // the interface arm walks a table and so ends in a block of its own, which the PHI below has to take
    // its incoming value from rather than from `read_block`
    llvm::Value *answer = nullptr;

    if (against_interface) {
        answer = gen_conformance_scan(handle, operand_box, *node.queried_type.get_complex_type());
    }
    else {
        llvm::Value *typeinfo = _ctx.builder->CreateLoad(
            llvm::PointerType::get(*_ctx.llvm_context, 0),
            gen_typeinfo_ptr(handle, operand_box),
            "typeinfo");

        // an interface has no layout of its own - it is never allocated and never lowered - so only this
        // arm has a second one to resolve, and it is resolved where it is read
        const ClassLayout queried_layout = _ctx.types->get_or_create_class_layout(
            node.queried_type.get_complex_type(), *_ctx.current_cmp_unit);

        // an address comparison against the class's own linkonce_odr global. exact identity, with no
        // numbering scheme to keep stable across modules - the linker already keeps one definition
        answer = _ctx.builder->CreateICmpEQ(typeinfo, queried_layout.typeinfo, "is_same");
    }

    llvm::BasicBlock *answer_block = _ctx.builder->GetInsertBlock();
    _ctx.builder->CreateBr(done_block);

    _ctx.builder->SetInsertPoint(done_block);
    llvm::PHINode *result = _ctx.builder->CreatePHI(i1, 2, "instanceof.result");
    result->addIncoming(llvm::ConstantInt::get(i1, 0), entry_block);
    result->addIncoming(answer, answer_block);

    _ctx.push(result);
}

llvm::Value *ClassCodegen::gen_conformance_scan(
    llvm::Value *handle, llvm::Type *box_type, const AST::ComplexType &interface)
{
    llvm::Type *i1 = llvm::Type::getInt1Ty(*_ctx.llvm_context);
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = llvm::PointerType::get(*_ctx.llvm_context, 0);

    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();
    llvm::GlobalVariable *wanted =
        _ctx.types->get_or_create_interface_identity(interface, *_ctx.current_cmp_unit);

    llvm::Value *typeinfo = _ctx.builder->CreateLoad(
        opaque_ptr, gen_typeinfo_ptr(handle, box_type), "typeinfo");

    // the descriptor's two slots: how many interfaces, and where the identities are. asked of the one
    // function that spells its shape, so the writer that mints it and this scan cannot disagree about
    // which slot is which - they used to build the type independently, four lines each
    auto *info_type = _ctx.types->typeinfo_llvm_type();

    llvm::Value *count = _ctx.builder->CreateLoad(
        i64,
        _ctx.builder->CreateStructGEP(
            info_type, typeinfo, ClassTypeInfo::conformance_count_index, "conformance_count_ptr"),
        "conformance_count");

    llvm::Value *table = _ctx.builder->CreateLoad(
        opaque_ptr,
        _ctx.builder->CreateStructGEP(
            info_type, typeinfo, ClassTypeInfo::conformances_index, "conformances_ptr"),
        "conformances");

    llvm::BasicBlock *read_block = _ctx.builder->GetInsertBlock();
    llvm::BasicBlock *loop_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "conforms.loop", function);
    llvm::BasicBlock *next_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "conforms.next", function);
    llvm::BasicBlock *exit_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "conforms.exit", function);

    // a count of zero is the common case - most classes conform to nothing - and it is also what guards
    // the null table, since `conformances` is only non-null when the count is not zero
    _ctx.builder->CreateCondBr(
        _ctx.builder->CreateICmpEQ(count, llvm::ConstantInt::get(i64, 0), "conforms.empty"),
        exit_block, loop_block);

    _ctx.builder->SetInsertPoint(loop_block);
    llvm::PHINode *index = _ctx.builder->CreatePHI(i64, 2, "conforms.i");
    index->addIncoming(llvm::ConstantInt::get(i64, 0), read_block);

    llvm::Value *slot = _ctx.builder->CreateGEP(opaque_ptr, table, index, "conforms.slot");
    llvm::Value *found = _ctx.builder->CreateLoad(opaque_ptr, slot, "conforms.entry");
    llvm::Value *hit = _ctx.builder->CreateICmpEQ(found, wanted, "conforms.hit");
    _ctx.builder->CreateCondBr(hit, exit_block, next_block);

    _ctx.builder->SetInsertPoint(next_block);
    llvm::Value *stepped = _ctx.builder->CreateAdd(index, llvm::ConstantInt::get(i64, 1), "conforms.i.next");
    index->addIncoming(stepped, next_block);
    _ctx.builder->CreateCondBr(
        _ctx.builder->CreateICmpULT(stepped, count, "conforms.more"), loop_block, exit_block);

    // three ways in and only one of them is a yes: the table was empty, the whole table was walked, or an
    // entry matched. a PHI rather than a mutable flag, so the answer is an SSA value like every other
    _ctx.builder->SetInsertPoint(exit_block);
    llvm::PHINode *result = _ctx.builder->CreatePHI(i1, 3, "conforms.result");
    result->addIncoming(llvm::ConstantInt::get(i1, 0), read_block);
    result->addIncoming(llvm::ConstantInt::get(i1, 1), loop_block);
    result->addIncoming(llvm::ConstantInt::get(i1, 0), next_block);

    return result;
}

llvm::Value *ClassCodegen::gen_retain(llvm::Value *handle, const AST::ValueType &class_type)
{
    const ClassLayout layout =
        _ctx.types->get_or_create_class_layout(class_type.get_complex_type(), *_ctx.current_cmp_unit);

    gen_strong_inc(handle, &layout, "retain");

    // the handle itself, unchanged. a retain is a side effect on the block, not a new value
    return handle;
}

void ClassCodegen::gen_strong_inc(llvm::Value *block, const ClassLayout *layout, const char *label)
{
    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    llvm::BasicBlock *bump_block =
        llvm::BasicBlock::Create(*_ctx.llvm_context, label, function);
    llvm::BasicBlock *done_block =
        llvm::BasicBlock::Create(*_ctx.llvm_context, std::string(label) + ".done", function);

    // null-safe: a class handle is nullable, so retaining one that holds nothing is `$a = $b` where
    // `$b` is null. skipping rather than trapping keeps null a first-class value of the type. for an
    // environment it is not even an edge case - a closure that captured nothing carries none at all,
    // which is the whole reason a callable is a fat pointer
    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(block), done_block, bump_block);

    _ctx.builder->SetInsertPoint(bump_block);

    // the count's address is only well defined once the block is known non-null, so it is taken here
    llvm::Value *strong_ptr = layout != nullptr ? gen_strong_ptr(block, *layout) : block;
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Value *count = _ctx.builder->CreateLoad(i64, strong_ptr, "strong");
    _ctx.builder->CreateStore(
        _ctx.builder->CreateAdd(count, llvm::ConstantInt::get(i64, 1), "strong.inc"), strong_ptr);
    _ctx.builder->CreateBr(done_block);

    _ctx.builder->SetInsertPoint(done_block);
}

llvm::Value *ClassCodegen::gen_strong_count(llvm::Value *handle, const AST::ValueType &class_type)
{
    const ClassLayout layout =
        _ctx.types->get_or_create_class_layout(class_type.get_complex_type(), *_ctx.current_cmp_unit);

    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    auto *load_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "refcount.load", function);
    auto *done_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "refcount.done", function);
    llvm::BasicBlock *null_block = _ctx.builder->GetInsertBlock();

    // null-safe for gen_strong_inc's reason, and the answer matters: **a null handle owns nothing, so
    // the count is 0.** that is what lets a copy-on-write check read as one condition - a `string`
    // holding static literal bytes has a null owner, is therefore not uniquely owned, and clones before
    // mutating, exactly as a shared one does. answering 1 would have it scribble on the binary
    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(handle), done_block, load_block);

    _ctx.builder->SetInsertPoint(load_block);

    // the count's address is only well defined once the handle is known non-null, as above
    llvm::Value *count = _ctx.builder->CreateLoad(i64, gen_strong_ptr(handle, layout), "strong");
    _ctx.builder->CreateBr(done_block);

    _ctx.builder->SetInsertPoint(done_block);

    llvm::PHINode *result = _ctx.builder->CreatePHI(i64, 2, "refcount");
    result->addIncoming(llvm::ConstantInt::get(i64, 0), null_block);
    result->addIncoming(count, load_block);

    return result;
}

void ClassCodegen::gen_release(llvm::Value *handle, const AST::ValueType &class_type)
{
    _ctx.builder->CreateCall(get_or_create_release_thunk(class_type), { handle });
}

llvm::Function *ClassCodegen::get_or_create_release_thunk(const AST::ValueType &class_type)
{
    const AST::ComplexType *complex = class_type.get_complex_type();

    // the mangled type token, so two classes of the same name in different namespaces get different
    // thunks and the same class reached from two units gets the same symbol
    const std::string name = "__eco_release_" + complex->mangled_token();

    if (llvm::Function *existing = _ctx.current_module()->getFunction(name)) {
        return existing;
    }

    // asked for only once the thunk is known to need building: this runs at every release site, and in
    // the steady state that is a map lookup and a name to throw away
    const ClassLayout layout = _ctx.types->get_or_create_class_layout(complex, *_ctx.current_cmp_unit);

    return build_release_thunk(name, &layout, complex);
}

llvm::Function *ClassCodegen::build_release_thunk(
    const std::string &name, const ClassLayout *layout, const AST::ComplexType *complex)
{
    llvm::Type *void_type = llvm::Type::getVoidTy(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = llvm::PointerType::get(*_ctx.llvm_context, 0);
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);

    llvm::Function *thunk = llvm::Function::Create(
        llvm::FunctionType::get(void_type, { opaque_ptr }, false),
        // linkonce_odr for the same reason the typeinfo global is: every unit that releases this block
        // emits its own definition, and the linker folds them
        llvm::GlobalValue::LinkOnceODRLinkage,
        name,
        _ctx.current_module());

    llvm::Value *handle = thunk->getArg(0);
    handle->setName(layout != nullptr ? "obj" : "env");

    // built with its own builder position, then restored - this is called from the middle of whatever
    // function asked for a release
    llvm::BasicBlock *saved_block = _ctx.builder->GetInsertBlock();
    auto saved_point = _ctx.builder->GetInsertPoint();

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", thunk);
    llvm::BasicBlock *dec_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "dec", thunk);
    llvm::BasicBlock *free_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "free", thunk);
    llvm::BasicBlock *return_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "done", thunk);

    _ctx.builder->SetInsertPoint(entry);
    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(handle), return_block, dec_block);

    _ctx.builder->SetInsertPoint(dec_block);

    // an environment passes no layout: its count is the block's first word, so the handle *is* the
    // count's address. see gen_callable_release
    llvm::Value *strong_ptr = layout != nullptr ? gen_strong_ptr(handle, *layout) : handle;
    llvm::Value *count = _ctx.builder->CreateLoad(i64, strong_ptr, "strong");
    llvm::Value *next = _ctx.builder->CreateSub(count, llvm::ConstantInt::get(i64, 1), "strong.dec");
    _ctx.builder->CreateStore(next, strong_ptr);
    _ctx.builder->CreateCondBr(
        _ctx.builder->CreateICmpEQ(next, llvm::ConstantInt::get(i64, 0), "is_last"),
        free_block,
        return_block);

    _ctx.builder->SetInsertPoint(free_block);

    // the payload's teardown, when there is any. the deinit takes `Foo&` - a pointer to a *slot*
    // holding a handle, which is the receiver shape every method and destructor uses - so the handle
    // is spilled to a slot to be addressed. two instructions to keep one receiver convention
    if (AST::FunctionDeclNode *deinit = complex != nullptr ? complex->deinit() : nullptr) {
        auto deinit_id = _ctx.current_cmp_unit->function_table.get_function_id(deinit);
        if (deinit_id == 0) {
            _ctx.types->create_llvm_func_decl(deinit, *_ctx.current_cmp_unit);
            deinit_id = _ctx.current_cmp_unit->function_table.get_function_id(deinit);
        }

        if (deinit_id == 0) {
            throw _ctx.error(fmt::format(
                "Class '{}' has a deinit that is not declared in compilation unit '{}'",
                complex->name.value_or("<anonymous>"),
                _ctx.current_cmp_unit->ast_module ? _ctx.current_cmp_unit->ast_module->name : "<unknown>"));
        }

        llvm::Value *slot = _ctx.builder->CreateAlloca(opaque_ptr, nullptr, "self_slot");
        _ctx.builder->CreateStore(handle, slot);
        _ctx.builder->CreateCall(
            _ctx.current_cmp_unit->function_table.get_llvm_function(deinit_id), { slot });
    }

    _ctx.builder->CreateCall(get_free(), { handle });
    _ctx.builder->CreateBr(return_block);

    _ctx.builder->SetInsertPoint(return_block);
    _ctx.builder->CreateRetVoid();

    if (saved_block != nullptr) {
        _ctx.builder->SetInsertPoint(saved_block, saved_point);
    }

    return thunk;
}

};
