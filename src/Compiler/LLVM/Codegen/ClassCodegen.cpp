#include "Compiler/LLVM/Codegen/ClassCodegen.h"
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
    return _ctx.current_module()->getOrInsertFunction(
        "malloc",
        llvm::FunctionType::get(
            llvm::PointerType::get(*_ctx.llvm_context, 0),
            { llvm::Type::getInt64Ty(*_ctx.llvm_context) },
            false));
}

llvm::FunctionCallee ClassCodegen::get_free()
{
    return _ctx.current_module()->getOrInsertFunction(
        "free",
        llvm::FunctionType::get(
            llvm::Type::getVoidTy(*_ctx.llvm_context),
            { llvm::PointerType::get(*_ctx.llvm_context, 0) },
            false));
}

llvm::Value *ClassCodegen::gen_strong_ptr(llvm::Value *handle, const ClassLayout &layout)
{
    return _ctx.builder->CreateStructGEP(layout.box, handle, ClassBox::strong_index, "strong_ptr");
}

void ClassCodegen::gen_class_alloc(AST::ClassAllocExprNode &node)
{
    const ClassLayout layout =
        _ctx.types->get_or_create_class_layout(node.class_type.get_complex_type(), *_ctx.current_cmp_unit);

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

    _ctx.push(handle);
}

void ClassCodegen::gen_retain_expr(AST::RetainExprNode &node)
{
    node.operand->accept(*_ctx.visitor);

    llvm::Value *handle = _ctx.pop();

    // the handle flows straight through - the retain is a side effect on the block, so the value this
    // expression hands its consumer is the one the operand produced
    _ctx.push(gen_retain(handle, node.result_type()));
}

void ClassCodegen::gen_release_stmt(AST::ReleaseNode &node)
{
    // read the slot *now* rather than trusting a handle captured earlier: between the declaration and
    // this release an assignment may have re-seated the variable, and it is the current reference the
    // scope owes
    LValue place = _ctx.lvalues->gen_lvalue(*node.target);

    gen_release(_ctx.lvalues->gen_load(place, "obj"), place.storage_type);
}

void ClassCodegen::gen_instanceof(AST::InstanceOfExprNode &node)
{
    llvm::Type *i1 = llvm::Type::getInt1Ty(*_ctx.llvm_context);

    // a class operand tested against a struct. the answer is knowable here and it is always no: a
    // struct has no block, so nothing could ever have written its identity into one
    if (!node.queried_type.is_class()) {
        node.operand->accept(*_ctx.visitor);
        _ctx.pop();
        _ctx.push(llvm::ConstantInt::get(i1, 0));
        return;
    }

    const ClassLayout operand_layout = _ctx.types->get_or_create_class_layout(
        node.operand->result_type().get_complex_type(), *_ctx.current_cmp_unit);

    const ClassLayout queried_layout = _ctx.types->get_or_create_class_layout(
        node.queried_type.get_complex_type(), *_ctx.current_cmp_unit);

    node.operand->accept(*_ctx.visitor);
    llvm::Value *handle = _ctx.pop();

    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    llvm::BasicBlock *entry_block = _ctx.builder->GetInsertBlock();
    llvm::BasicBlock *read_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "instanceof", function);
    llvm::BasicBlock *done_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "instanceof.done", function);

    // null is not an instance of anything, and there is no block to read the identity out of - so the
    // load has to be guarded rather than merely answered false afterwards
    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(handle), done_block, read_block);

    _ctx.builder->SetInsertPoint(read_block);
    llvm::Value *typeinfo = _ctx.builder->CreateLoad(
        llvm::PointerType::get(*_ctx.llvm_context, 0),
        _ctx.builder->CreateStructGEP(
            operand_layout.box, handle, ClassBox::typeinfo_index, "typeinfo_ptr"),
        "typeinfo");

    // an address comparison against the class's own linkonce_odr global. exact identity, with no
    // numbering scheme to keep stable across modules - the linker already keeps one definition
    llvm::Value *is_same = _ctx.builder->CreateICmpEQ(typeinfo, queried_layout.typeinfo, "is_same");
    _ctx.builder->CreateBr(done_block);

    _ctx.builder->SetInsertPoint(done_block);
    llvm::PHINode *result = _ctx.builder->CreatePHI(i1, 2, "instanceof.result");
    result->addIncoming(llvm::ConstantInt::get(i1, 0), entry_block);
    result->addIncoming(is_same, read_block);

    _ctx.push(result);
}

llvm::Value *ClassCodegen::gen_retain(llvm::Value *handle, const AST::ValueType &class_type)
{
    const ClassLayout layout =
        _ctx.types->get_or_create_class_layout(class_type.get_complex_type(), *_ctx.current_cmp_unit);

    llvm::Function *function = _ctx.builder->GetInsertBlock()->getParent();

    llvm::BasicBlock *bump_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "retain", function);
    llvm::BasicBlock *done_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "retain.done", function);

    // null-safe: a class handle is nullable, so retaining one that holds nothing is `$a = $b` where
    // `$b` is null. skipping rather than trapping keeps null a first-class value of the type
    _ctx.builder->CreateCondBr(_ctx.builder->CreateIsNull(handle), done_block, bump_block);

    _ctx.builder->SetInsertPoint(bump_block);
    llvm::Value *strong_ptr = gen_strong_ptr(handle, layout);
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Value *count = _ctx.builder->CreateLoad(i64, strong_ptr, "strong");
    _ctx.builder->CreateStore(
        _ctx.builder->CreateAdd(count, llvm::ConstantInt::get(i64, 1), "strong.inc"), strong_ptr);
    _ctx.builder->CreateBr(done_block);

    _ctx.builder->SetInsertPoint(done_block);

    // the handle itself, unchanged. a retain is a side effect on the block, not a new value
    return handle;
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

    llvm::Type *void_type = llvm::Type::getVoidTy(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = llvm::PointerType::get(*_ctx.llvm_context, 0);
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);

    llvm::Function *thunk = llvm::Function::Create(
        llvm::FunctionType::get(void_type, { opaque_ptr }, false),
        // linkonce_odr for the same reason the typeinfo global is: every unit that releases this class
        // emits its own definition, and the linker folds them
        llvm::GlobalValue::LinkOnceODRLinkage,
        name,
        _ctx.current_module());

    llvm::Value *handle = thunk->getArg(0);
    handle->setName("obj");

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
    llvm::Value *strong_ptr = gen_strong_ptr(handle, layout);
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
    if (AST::FunctionDeclNode *deinit = complex->deinit()) {
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
