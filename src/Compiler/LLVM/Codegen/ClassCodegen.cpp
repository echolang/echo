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

llvm::Value *ClassCodegen::gen_header_ptr(
    llvm::Value *handle, llvm::Type *box_type, unsigned index, const llvm::Twine &name)
{
    return _ctx.builder->CreateStructGEP(box_type, handle, index, name);
}

llvm::Type *ClassCodegen::header_box_type(const ClassLayout *layout)
{
    return layout != nullptr ? static_cast<llvm::Type *>(layout->box) : _ctx.types->class_header_llvm_type();
}

// what to call the count in the IR, keyed on which one it is rather than on the operation reaching it.
// so `strong` and `strong.inc` read the same whether a retain, an upgrade or an interface widening
// produced them - which is what makes an `--- IR --->` check able to name a count at all
static const char *count_name(unsigned index)
{
    return index == ClassBox::weak_index ? "weak" : "strong";
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
        llvm::ConstantInt::get(i64, 1),
        gen_header_ptr(handle, layout.box, ClassBox::strong_index, "strong_ptr"));

    // and one weak reference, held by the strong ones collectively rather than by any handle a program
    // can name. it is what keeps the block readable for as long as anybody owns the object, and the
    // release that takes the strong count to zero is what gives it back - so this 1 is the reason a
    // teardown and a free are two moments instead of one. see Codegen/ClassLayout.h
    _ctx.builder->CreateStore(
        llvm::ConstantInt::get(i64, 1),
        gen_header_ptr(handle, layout.box, ClassBox::weak_index, "weak_ptr"));

    _ctx.builder->CreateStore(
        layout.typeinfo,
        gen_header_ptr(handle, layout.box, ClassBox::typeinfo_index, "typeinfo_ptr"));

    return handle;
}

llvm::Value *ClassCodegen::gen_callable_retain(llvm::Value *callable)
{
    // a null layout: an environment's block is built by gen_class_box_alloc like any other, so its header
    // is the shared one, and that is all a count needs. the layout is exactly what a callable does not know
    gen_count_inc(
        _ctx.builder->CreateExtractValue(callable, 1, "retain.env"),
        nullptr, ClassBox::strong_index, "env.retain");

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

    // no layout and no deinit: the header is the shared one, and an environment holds no owning capture,
    // so the block is all there is to give back
    return build_release_thunk(name, nullptr, nullptr);
}

llvm::Function *ClassCodegen::get_or_create_weak_release_thunk()
{
    const std::string name = "__eco_weak_release";

    if (llvm::Function *existing = _ctx.current_module()->getFunction(name)) {
        return existing;
    }

    // through the shared header rather than a layout, and that is not a compromise: nothing about this
    // decrement is per class, which is why there is one of these and not one per type
    return build_count_release_thunk(
        declare_release_thunk(name, "obj"),
        _ctx.types->class_header_llvm_type(), ClassBox::weak_index, "free",
        [this](llvm::Value *handle) {
            // **the only free in the runtime.** the payload is already torn down by the time any path
            // arrives here - the strong release ran the deinit before dropping its collective weak
            // reference - so this gives back memory and reads nothing
            _ctx.builder->CreateCall(get_free(), { handle });
        });
}

llvm::Value *ClassCodegen::gen_weak_of(llvm::Value *handle, const AST::ValueType &class_type)
{
    const ClassLayout layout =
        _ctx.types->get_or_create_class_layout(class_type.get_complex_type(), *_ctx.current_cmp_unit);

    gen_count_inc(handle, &layout, ClassBox::weak_index, "weak.retain");

    // the same address, differently typed. a weak handle *is* the block pointer - what makes it weak is
    // which word it moved and that reading it needs an upgrade, not a second representation
    return handle;
}

llvm::Value *ClassCodegen::gen_strong_upgrade(llvm::Value *weak_handle, const AST::ValueType &class_type)
{
    const ClassLayout layout =
        _ctx.types->get_or_create_class_layout(class_type.get_complex_type(), *_ctx.current_cmp_unit);

    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = llvm::PointerType::get(*_ctx.llvm_context, 0);
    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    auto *read_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "upgrade.read", function);
    auto *live_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "upgrade.live", function);
    auto *done_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "upgrade.done", function);
    llvm::BasicBlock *null_block = _ctx.builder->GetInsertBlock();

    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(weak_handle), done_block, read_block);

    // **the load this whole feature is for.** the block is readable because this weak reference is holding
    // it, and the strong count in it says whether the payload is still there. a program with no weak count
    // could not ask - it would have to read the payload to find out, which is the dangle
    _ctx.builder->SetInsertPoint(read_block);
    llvm::Value *strong = _ctx.builder->CreateLoad(
        i64,
        gen_header_ptr(weak_handle, layout.box, ClassBox::strong_index, "strong_ptr"),
        "strong");
    _ctx.builder->CreateCondBr(
        _ctx.builder->CreateICmpEQ(strong, llvm::ConstantInt::get(i64, 0), "upgrade.dead"),
        done_block,
        live_block);

    // one more owner, and it is this one that makes the handed-back handle safe to read through: the
    // count cannot reach zero while the caller holds it
    _ctx.builder->SetInsertPoint(live_block);
    _ctx.builder->CreateStore(
        _ctx.builder->CreateAdd(strong, llvm::ConstantInt::get(i64, 1), "strong.inc"),
        gen_header_ptr(weak_handle, layout.box, ClassBox::strong_index, "strong_ptr"));
    _ctx.builder->CreateBr(done_block);

    _ctx.builder->SetInsertPoint(done_block);

    // two ways to be absent and one to be there, so the result is `T?` - null for a weak that held nothing
    // and for one whose object is gone, which are the same answer to the only question a caller can ask
    llvm::PHINode *result = _ctx.builder->CreatePHI(opaque_ptr, 3, "upgraded");
    llvm::Constant *null_handle = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(opaque_ptr));
    result->addIncoming(null_handle, null_block);
    result->addIncoming(null_handle, read_block);
    result->addIncoming(weak_handle, live_block);

    return result;
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

    // **the arm that makes a weak an owner without a single new AST node.** the ownership pass wrote an
    // ordinary RetainExprNode over a weak-typed place, and this is where "which count" is finally asked -
    // of the type, at the one site that emits the instruction. so the copy taxonomy, the drop walk and
    // the assignment order upstream all stayed exactly as they were
    if (type.is_weak()) {
        return gen_weak_of(value, type.weak_target());
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

    // the mirror of the retain arm above. one thunk for every class, because giving back a weak reference
    // runs no deinit and reads no property - see get_or_create_weak_release_thunk
    if (type.is_weak()) {
        _ctx.builder->CreateCall(get_or_create_weak_release_thunk(), { value });
        return;
    }

    gen_release(value, type);
}

llvm::Value *ClassCodegen::gen_iface_retain(llvm::Value *erased)
{
    // an erased value is `{ object, vtable }` and only the object is counted. the *count* lives in the
    // object's own block, exactly where a class handle's does, so this needs no layout: gen_count_inc
    // over a null layout reaches it through the header every box shares
    llvm::Value *object = _ctx.builder->CreateExtractValue(erased, { IfaceValue::object_index }, "iface.obj");
    gen_count_inc(object, nullptr, ClassBox::strong_index, "iface");

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
            gen_header_ptr(handle, operand_box, ClassBox::typeinfo_index, "typeinfo_ptr"),
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
        opaque_ptr,
        gen_header_ptr(handle, box_type, ClassBox::typeinfo_index, "typeinfo_ptr"),
        "typeinfo");

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

    gen_count_inc(handle, &layout, ClassBox::strong_index, "retain");

    // the handle itself, unchanged. a retain is a side effect on the block, not a new value
    return handle;
}

void ClassCodegen::gen_count_inc(
    llvm::Value *block, const ClassLayout *layout, unsigned index, const char *label)
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
    const std::string name = count_name(index);
    llvm::Value *count_ptr = gen_header_ptr(block, header_box_type(layout), index, name + "_ptr");
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Value *count = _ctx.builder->CreateLoad(i64, count_ptr, name);
    _ctx.builder->CreateStore(
        _ctx.builder->CreateAdd(count, llvm::ConstantInt::get(i64, 1), name + ".inc"), count_ptr);
    _ctx.builder->CreateBr(done_block);

    _ctx.builder->SetInsertPoint(done_block);
}

llvm::Value *ClassCodegen::gen_count(
    llvm::Value *handle, const AST::ValueType &class_type, unsigned index)
{
    const ClassLayout layout =
        _ctx.types->get_or_create_class_layout(class_type.get_complex_type(), *_ctx.current_cmp_unit);

    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    // derived from the index rather than passed beside it: the caller used to hand in both, which is two
    // arguments it had to keep consistent and one more place the two counts could be named apart
    const std::string label = std::string(count_name(index)) + "count";

    auto *load_block = llvm::BasicBlock::Create(*_ctx.llvm_context, label + ".load", function);
    auto *done_block = llvm::BasicBlock::Create(*_ctx.llvm_context, label + ".done", function);
    llvm::BasicBlock *null_block = _ctx.builder->GetInsertBlock();

    // null-safe for gen_count_inc's reason, and the answer matters: **a null handle owns nothing, so
    // the count is 0.** that is what lets a copy-on-write check read as one condition - a `string`
    // holding static literal bytes has a null owner, is therefore not uniquely owned, and clones before
    // mutating, exactly as a shared one does. answering 1 would have it scribble on the binary
    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(handle), done_block, load_block);

    _ctx.builder->SetInsertPoint(load_block);

    // the count's address is only well defined once the handle is known non-null, as above
    const std::string name = count_name(index);
    llvm::Value *count = _ctx.builder->CreateLoad(
        i64, gen_header_ptr(handle, layout.box, index, name + "_ptr"), name);
    _ctx.builder->CreateBr(done_block);

    _ctx.builder->SetInsertPoint(done_block);

    llvm::PHINode *result = _ctx.builder->CreatePHI(i64, 2, label);
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
    // **declared before the weak thunk is asked for, and resolved before this one's blocks exist.** both
    // halves of that ordering are load-bearing: this symbol has to be appended to the module first, so a
    // reader sees the call site above the definition it names, and asking for the weak thunk from inside
    // the zero arm below would save and restore an insert point into a block that is mid-construction
    llvm::Function *thunk = declare_release_thunk(name, layout != nullptr ? "obj" : "env");
    llvm::Function *weak_release = get_or_create_weak_release_thunk();

    // an environment passes no layout, so the word is reached through the header every box shares. it used
    // to treat the handle *as* the count's address, which was true only while __strong was the first word
    return build_count_release_thunk(
        thunk, header_box_type(layout), ClassBox::strong_index, "dead",
        [this, complex, weak_release](llvm::Value *handle) {
            // **the object is dead here, the block is not.** the payload's teardown runs, and then the one
            // weak reference the strong ones collectively held is given back - which frees the block only
            // if no `weak<T>` is still holding it. so a weak handle outlives the object it names and can
            // say so, rather than pointing at memory that has been handed back to malloc
            gen_deinit_call(complex, handle);
            _ctx.builder->CreateCall(weak_release, { handle });
        });
}

// the payload's teardown, when there is any. the deinit takes `Foo&` - a pointer to a *slot* holding a
// handle, which is the receiver shape every method and destructor uses - so the handle is spilled to a
// slot to be addressed. two instructions to keep one receiver convention
void ClassCodegen::gen_deinit_call(const AST::ComplexType *complex, llvm::Value *handle)
{
    AST::FunctionDeclNode *deinit = complex != nullptr ? complex->deinit() : nullptr;

    if (deinit == nullptr) {
        return;
    }

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

    llvm::Value *slot = _ctx.builder->CreateAlloca(
        llvm::PointerType::get(*_ctx.llvm_context, 0), nullptr, "self_slot");
    _ctx.builder->CreateStore(handle, slot);
    _ctx.builder->CreateCall(
        _ctx.current_cmp_unit->function_table.get_llvm_function(deinit_id), { slot });
}

llvm::Function *ClassCodegen::declare_release_thunk(const std::string &name, const char *handle_name)
{
    llvm::Type *void_type = llvm::Type::getVoidTy(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = llvm::PointerType::get(*_ctx.llvm_context, 0);

    llvm::Function *thunk = llvm::Function::Create(
        llvm::FunctionType::get(void_type, { opaque_ptr }, false),
        // linkonce_odr for the same reason the typeinfo global is: every unit that releases this block
        // emits its own definition, and the linker folds them
        llvm::GlobalValue::LinkOnceODRLinkage,
        name,
        _ctx.current_module());

    thunk->getArg(0)->setName(handle_name);

    return thunk;
}

llvm::Function *ClassCodegen::build_count_release_thunk(
    llvm::Function *thunk,
    llvm::Type *box_type,
    unsigned count_index,
    const char *zero_block_name,
    llvm::function_ref<void(llvm::Value *handle)> on_zero)
{
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Value *handle = thunk->getArg(0);

    // built with its own builder position, then restored - this is called from the middle of whatever
    // function asked for a release
    llvm::BasicBlock *saved_block = _ctx.builder->GetInsertBlock();
    auto saved_point = _ctx.builder->GetInsertPoint();

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", thunk);
    llvm::BasicBlock *dec_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "dec", thunk);
    llvm::BasicBlock *zero_block = llvm::BasicBlock::Create(*_ctx.llvm_context, zero_block_name, thunk);
    llvm::BasicBlock *return_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "done", thunk);

    _ctx.builder->SetInsertPoint(entry);
    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(handle), return_block, dec_block);

    _ctx.builder->SetInsertPoint(dec_block);

    // the labels come off the index, so an arm cannot name one word while the GEP reaches another
    const std::string count = count_name(count_index);

    llvm::Value *count_ptr = gen_header_ptr(handle, box_type, count_index, count + "_ptr");
    llvm::Value *current = _ctx.builder->CreateLoad(i64, count_ptr, count);
    llvm::Value *next = _ctx.builder->CreateSub(current, llvm::ConstantInt::get(i64, 1), count + ".dec");
    _ctx.builder->CreateStore(next, count_ptr);
    _ctx.builder->CreateCondBr(
        _ctx.builder->CreateICmpEQ(next, llvm::ConstantInt::get(i64, 0), "is_last_" + count),
        zero_block,
        return_block);

    _ctx.builder->SetInsertPoint(zero_block);
    on_zero(handle);
    _ctx.builder->CreateBr(return_block);

    _ctx.builder->SetInsertPoint(return_block);
    _ctx.builder->CreateRetVoid();

    if (saved_block != nullptr) {
        _ctx.builder->SetInsertPoint(saved_block, saved_point);
    }

    return thunk;
}
};
