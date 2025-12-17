#include "Compiler/LLVM/Codegen/AbortCodegen.h"

#include "AST/ASTBuiltin.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "Compiler/LLVM/Codegen/DebugInfoCodegen.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <fmt/core.h>

#include <cassert>

namespace Compiler::LLVM
{

llvm::FunctionCallee AbortCodegen::get_exit()
{
    llvm::FunctionCallee callee = _ctx.libc_callee(
        "exit",
        llvm::Type::getVoidTy(*_ctx.llvm_context),
        { llvm::Type::getInt32Ty(*_ctx.llvm_context) });

    // so the `unreachable` after the call is a fact the optimizer can use rather than an assertion
    // it has to take on faith
    if (auto *func = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
        func->addFnAttr(llvm::Attribute::NoReturn);
    }

    return callee;
}

llvm::Function *AbortCodegen::get_or_create_abort_thunk()
{
    const std::string name = "__eco_abort";

    if (llvm::Function *existing = _ctx.current_module()->getFunction(name)) {
        return existing;
    }

    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();

    llvm::Function *thunk = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(*_ctx.llvm_context), { opaque_ptr, i64 }, false),
        // linkonce_odr for the same reason the release thunks are: every unit that can stop emits
        // its own definition, and the linker folds them
        llvm::GlobalValue::LinkOnceODRLinkage,
        name,
        _ctx.current_module());

    thunk->addFnAttr(llvm::Attribute::NoReturn);

    llvm::Value *msg = thunk->getArg(0);
    msg->setName("msg");
    llvm::Value *len = thunk->getArg(1);
    len->setName("len");

    // built with its own builder position, restored on the way out - this is called from the middle
    // of whatever function asked to stop. the guard also covers the "not inside a function yet"
    // case, which a hand-rolled save/restore had to special-case
    llvm::IRBuilderBase::InsertPointGuard guard(*_ctx.builder);

    _ctx.set_insert_point(llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", thunk));

    // stdout first. without this a program that echoes and then dies prints the two in the wrong
    // order, because stdout is buffered and the write below is not
    _ctx.builder->CreateCall(
        _ctx.libc_callee("fflush", i32, { opaque_ptr }),
        { llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(opaque_ptr)) });

    // write(2, ...) rather than fprintf(stderr, ...): the `stderr` global is spelled differently
    // per platform (`__stderrp` on Darwin) and is not portably addressable from IR
    _ctx.builder->CreateCall(
        _ctx.libc_callee("write", i64, { i32, opaque_ptr, i64 }),
        { llvm::ConstantInt::get(i32, 2), msg, len });

    _ctx.builder->CreateCall(get_exit(), { llvm::ConstantInt::get(i32, 1) });
    _ctx.builder->CreateUnreachable();

    return thunk;
}

void AbortCodegen::gen_abort(
    const std::string &headline,
    const std::string &detail,
    const std::string &location
)
{
    // the one message shape, so the three stop sites cannot render differently
    const std::string message = detail.empty()
        ? fmt::format("{}\n  at {}\n", headline, location)
        : fmt::format("{}: {}\n  at {}\n", headline, detail, location);

    // unnamed: a named global forces makeUniqueName to spin out abort.msg.1, .2, ... and puts every
    // one of them in the module's symbol table, which for an assert inside a generic is once per
    // instantiation
    llvm::Value *msg = _ctx.builder->CreateGlobalStringPtr(message, "");

    _ctx.builder->CreateCall(get_or_create_abort_thunk(), {
        msg,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*_ctx.llvm_context), message.size()),
    });

    _ctx.builder->CreateUnreachable();
}

void AbortCodegen::gen_exit(llvm::Value *code)
{
    _ctx.builder->CreateCall(get_exit(), { code });
    _ctx.builder->CreateUnreachable();
}

void AbortCodegen::gen_abort_if(
    llvm::Value *condition,
    const std::string &headline,
    const std::string &detail,
    const std::string &location
)
{
    llvm::Function *fn = _ctx.builder->GetInsertBlock()->getParent();

    llvm::BasicBlock *abort_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "abort", fn);
    llvm::BasicBlock *ok_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "abort.ok", fn);

    _ctx.builder->CreateCondBr(condition, abort_block, ok_block);

    _ctx.set_insert_point(abort_block);
    gen_abort(headline, detail, location);

    // the caller carries on where the program did not stop
    _ctx.set_insert_point(ok_block);
}

std::string AbortCodegen::location_of(const AST::FunctionCallExprNode &node) const
{
    return fmt::format("{}:{}", _ctx.current_file_name(), node.token_function_name.line());
}

std::string AbortCodegen::detail_of(const AST::FunctionCallExprNode &node) const
{
    if (node.decl == nullptr || !node.decl->is_builtin()) {
        return "";
    }

    // the same index AST::TypeChecker validated the literal at - spelled here as well, the two
    // could check one argument and fold another
    const auto index = AST::builtin_message_index(
        AST::builtin_kind_for(node.decl->builtin.value()));

    if (!index.has_value() || node.arguments.size() <= *index) {
        return "";
    }

    // the checker rejected anything without an answer here, so a missing one is a compiler bug
    // rather than a message that quietly loses its text
    const auto text = AST::literal_string_value(node.arguments[*index]);
    assert(text.has_value() && "abort message survived the type checker without being a literal");

    return text.value_or("");
}

};
