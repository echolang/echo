#include "Compiler/LLVM/Codegen/AbortCodegen.h"

#include "AST/ASTBuiltin.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTFile.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "Compiler/LLVM/Codegen/DebugInfoCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"
#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

#include <algorithm>
#include <vector>

#include <fmt/core.h>

namespace Compiler::LLVM
{

static std::string file_basename(const TokenReference &token)
{
    if (token.file() == nullptr) {
        return "<unknown>";
    }

    return token.file()->get_path().filename().string();
}

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

bool AbortCodegen::hooks_enabled() const
{
    return _ctx.core_types().has(AST::CoreTypeKind::t_crash_info);
}

llvm::GlobalVariable *AbortCodegen::hook_global()
{
    return _ctx.get_or_create_odr_global("__eco_crash_hook", _ctx.opaque_ptr_type());
}

llvm::Function *AbortCodegen::get_or_create_abort_thunk()
{
    const std::string name = "__eco_abort";

    if (llvm::Function *existing = _ctx.current_module()->getFunction(name)) {
        return existing;
    }

    llvm::Type *void_ty = llvm::Type::getVoidTy(*_ctx.llvm_context);
    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();

    llvm::FunctionType *type = llvm::FunctionType::get(
        void_ty,
        { opaque_ptr, i64, opaque_ptr, i64, opaque_ptr, i64, i32, opaque_ptr, i64 },
        false);

    llvm::Function *thunk = llvm::Function::Create(
        type,
        // linkonce_odr for the same reason the release thunks are: every unit that can stop emits
        // its own definition, and the linker folds them
        llvm::GlobalValue::LinkOnceODRLinkage,
        name,
        _ctx.current_module());

    thunk->addFnAttr(llvm::Attribute::NoReturn);

    llvm::IRBuilderBase::InsertPointGuard guard(*_ctx.builder);
    _ctx.set_insert_point(llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", thunk));

    emit_thunk(thunk);
    return thunk;
}

void AbortCodegen::write_pieces(
    llvm::Value *headline, llvm::Value *headline_len,
    llvm::Value *message, llvm::Value *message_len,
    llvm::Value *file, llvm::Value *file_len,
    llvm::Value *line, llvm::Value *line_len
)
{
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);

    write_stderr(headline, headline_len);

    llvm::Function *parent = _ctx.builder->GetInsertBlock()->getParent();
    llvm::BasicBlock *with_msg = llvm::BasicBlock::Create(*_ctx.llvm_context, "crash.detail", parent);
    llvm::BasicBlock *after_msg = llvm::BasicBlock::Create(*_ctx.llvm_context, "crash.loc", parent);

    _ctx.builder->CreateCondBr(
        _ctx.builder->CreateICmpNE(message_len, llvm::ConstantInt::get(i64, 0), "crash.has_msg"),
        with_msg,
        after_msg);

    _ctx.set_insert_point(with_msg);
    write_stderr(_ctx.builder->CreateGlobalStringPtr(": ", ".crash.sep"), llvm::ConstantInt::get(i64, 2));
    write_stderr(message, message_len);
    _ctx.builder->CreateBr(after_msg);

    _ctx.set_insert_point(after_msg);
    write_stderr(_ctx.builder->CreateGlobalStringPtr("\n  at ", ".crash.sep"), llvm::ConstantInt::get(i64, 6));
    write_stderr(file, file_len);
    write_stderr(_ctx.builder->CreateGlobalStringPtr(":", ".crash.sep"), llvm::ConstantInt::get(i64, 1));
    write_stderr(line, line_len);
    write_stderr(_ctx.builder->CreateGlobalStringPtr("\n", ".crash.sep"), llvm::ConstantInt::get(i64, 1));
}

void AbortCodegen::crash_llvm_types(llvm::StructType *&info_ty, llvm::StructType *&view_ty)
{
    const AST::CoreCrashInfoLayout &layout = _ctx.core_crash_info_layout();

    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();
    llvm::Type *len_ty = llvm::Type::getIntNTy(
        *_ctx.llvm_context, static_cast<unsigned>(_ctx.layout().getPointerSizeInBits()));
    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);

    const size_t view_n = std::max(layout.view_bytes_index, layout.view_size_index) + 1;
    std::vector<llvm::Type *> view_fields(view_n, opaque_ptr);
    view_fields[layout.view_bytes_index] = opaque_ptr;
    view_fields[layout.view_size_index] = len_ty;
    view_ty = llvm::StructType::get(*_ctx.llvm_context, view_fields);

    const size_t info_n = std::max({
        layout.headline_index,
        layout.message_index,
        layout.file_index,
        layout.line_index,
    }) + 1;
    std::vector<llvm::Type *> info_fields(info_n, view_ty);
    info_fields[layout.headline_index] = view_ty;
    info_fields[layout.message_index] = view_ty;
    info_fields[layout.file_index] = view_ty;
    info_fields[layout.line_index] = i32;
    info_ty = llvm::StructType::get(*_ctx.llvm_context, info_fields);
}

void AbortCodegen::emit_thunk(llvm::Function *thunk)
{
    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();

    thunk->getArg(0)->setName("headline");
    thunk->getArg(1)->setName("headline.len");
    thunk->getArg(2)->setName("msg");
    thunk->getArg(3)->setName("msg.len");
    thunk->getArg(4)->setName("file");
    thunk->getArg(5)->setName("file.len");
    thunk->getArg(6)->setName("line");
    thunk->getArg(7)->setName("line.text");
    thunk->getArg(8)->setName("line.len");

    auto print_pieces = [&]() {
        write_pieces(
            thunk->getArg(0), thunk->getArg(1),
            thunk->getArg(2), thunk->getArg(3),
            thunk->getArg(4), thunk->getArg(5),
            thunk->getArg(7), thunk->getArg(8));
    };

    if (!hooks_enabled()) {
        flush_stdout();
        print_pieces();
        _ctx.builder->CreateCall(get_exit(), { llvm::ConstantInt::get(i32, 1) });
        _ctx.builder->CreateUnreachable();
        return;
    }

    const AST::CoreCrashInfoLayout &info_layout = _ctx.core_crash_info_layout();

    llvm::StructType *info_ty = nullptr;
    llvm::StructType *view_ty = nullptr;
    crash_llvm_types(info_ty, view_ty);

    llvm::Value *info = _ctx.entry_alloca(info_ty, "crash.info");

    store_view(info, info_ty, view_ty, info_layout.headline_index, thunk->getArg(0), thunk->getArg(1));
    store_view(info, info_ty, view_ty, info_layout.message_index, thunk->getArg(2), thunk->getArg(3));
    store_view(info, info_ty, view_ty, info_layout.file_index, thunk->getArg(4), thunk->getArg(5));

    llvm::Value *line_slot = _ctx.builder->CreateStructGEP(
        info_ty, info, static_cast<unsigned>(info_layout.line_index), "crash.line.slot");
    _ctx.builder->CreateStore(thunk->getArg(6), line_slot);

    flush_stdout();

    // steal the hook so a hook that dies cannot recurse into itself
    llvm::Value *fn = _ctx.builder->CreateLoad(opaque_ptr, hook_global(), "crash.hook");
    _ctx.builder->CreateStore(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(opaque_ptr)), hook_global());

    llvm::Function *parent = thunk;
    llvm::BasicBlock *hooked = llvm::BasicBlock::Create(*_ctx.llvm_context, "crash.hooked", parent);
    llvm::BasicBlock *printed = llvm::BasicBlock::Create(*_ctx.llvm_context, "crash.printed", parent);
    llvm::BasicBlock *done = llvm::BasicBlock::Create(*_ctx.llvm_context, "crash.exit", parent);

    _ctx.builder->CreateCondBr(
        _ctx.builder->CreateICmpNE(
            fn, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(opaque_ptr)), "crash.has_hook"),
        hooked,
        printed);

    _ctx.set_insert_point(hooked);
    llvm::FunctionType *hook_ty = llvm::FunctionType::get(
        llvm::Type::getVoidTy(*_ctx.llvm_context), { opaque_ptr }, false);
    _ctx.builder->CreateCall(hook_ty, fn, { info });
    _ctx.builder->CreateBr(done);

    _ctx.set_insert_point(printed);
    print_pieces();
    _ctx.builder->CreateBr(done);

    _ctx.set_insert_point(done);
    _ctx.builder->CreateCall(get_exit(), { llvm::ConstantInt::get(i32, 1) });
    _ctx.builder->CreateUnreachable();
}

void AbortCodegen::store_view(
    llvm::Value *info,
    llvm::StructType *info_ty,
    llvm::StructType *view_ty,
    size_t field,
    llvm::Value *bytes,
    llvm::Value *len
)
{
    llvm::Value *view = _ctx.builder->CreateStructGEP(
        info_ty, info, static_cast<unsigned>(field), "crash.view");

    const AST::CoreCrashInfoLayout &layout = _ctx.core_crash_info_layout();

    _ctx.builder->CreateStore(
        bytes,
        _ctx.builder->CreateStructGEP(view_ty, view, static_cast<unsigned>(layout.view_bytes_index)));
    _ctx.builder->CreateStore(
        len,
        _ctx.builder->CreateStructGEP(view_ty, view, static_cast<unsigned>(layout.view_size_index)));
}

void AbortCodegen::emit_default_print_body(llvm::Value *info)
{
    const AST::CoreCrashInfoLayout &info_layout = _ctx.core_crash_info_layout();

    const AST::ValueType info_type = _ctx.core_types().type(AST::CoreTypeKind::t_crash_info);
    llvm::StructType *info_ty = llvm::cast<llvm::StructType>(
        _ctx.types->get_llvm_type(info_type, *_ctx.current_cmp_unit));
    const AST::ValueType view_type = _ctx.core_types().string_view_type();
    llvm::Type *view_ty = _ctx.types->get_llvm_type(view_type, *_ctx.current_cmp_unit);

    auto load_view = [&](size_t field, const char *prefix) {
        llvm::Value *slot = _ctx.builder->CreateStructGEP(
            info_ty, info, static_cast<unsigned>(field), prefix);
        llvm::Value *view = _ctx.builder->CreateLoad(view_ty, slot, prefix);
        return CodegenContext::StringWindow{
            _ctx.builder->CreateExtractValue(
                view, { static_cast<unsigned>(info_layout.view_bytes_index) },
                fmt::format("{}bytes", prefix)),
            _ctx.builder->CreateExtractValue(
                view, { static_cast<unsigned>(info_layout.view_size_index) },
                fmt::format("{}size", prefix)),
        };
    };

    const auto headline = load_view(info_layout.headline_index, "crash.headline.");
    const auto message = load_view(info_layout.message_index, "crash.msg.");
    const auto file = load_view(info_layout.file_index, "crash.file.");

    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Type *i8 = llvm::Type::getInt8Ty(*_ctx.llvm_context);
    llvm::Type *buf_ty = llvm::ArrayType::get(i8, 16);

    llvm::Value *line = _ctx.builder->CreateLoad(
        i32,
        _ctx.builder->CreateStructGEP(
            info_ty, info, static_cast<unsigned>(info_layout.line_index), "crash.line.slot"),
        "crash.line");

    // snprintf's variadic ABI is not worth taking on the crash path; a 16-byte itoa is
    llvm::Value *buf = _ctx.entry_alloca(buf_ty, "crash.itoa");
    llvm::Value *zero = llvm::ConstantInt::get(i64, 0);
    llvm::Value *n64 = _ctx.builder->CreateZExt(line, i64, "crash.line64");
    llvm::Value *idx = llvm::ConstantInt::get(i64, 16);

    llvm::BasicBlock *pred = _ctx.builder->GetInsertBlock();
    llvm::Function *parent = pred->getParent();
    llvm::BasicBlock *loop = llvm::BasicBlock::Create(*_ctx.llvm_context, "crash.itoa.loop", parent);
    llvm::BasicBlock *done = llvm::BasicBlock::Create(*_ctx.llvm_context, "crash.itoa.done", parent);
    _ctx.builder->CreateBr(loop);

    _ctx.set_insert_point(loop);
    llvm::PHINode *rest = _ctx.builder->CreatePHI(i64, 2, "crash.rest");
    llvm::PHINode *at = _ctx.builder->CreatePHI(i64, 2, "crash.at");
    rest->addIncoming(n64, pred);
    at->addIncoming(idx, pred);

    llvm::Value *next = _ctx.builder->CreateSub(at, llvm::ConstantInt::get(i64, 1), "crash.next");
    llvm::Value *digit = _ctx.builder->CreateURem(rest, llvm::ConstantInt::get(i64, 10));
    _ctx.builder->CreateStore(
        _ctx.builder->CreateTrunc(
            _ctx.builder->CreateAdd(digit, llvm::ConstantInt::get(i64, '0')), i8),
        _ctx.builder->CreateInBoundsGEP(buf_ty, buf, { zero, next }));
    llvm::Value *more = _ctx.builder->CreateUDiv(rest, llvm::ConstantInt::get(i64, 10));
    rest->addIncoming(more, loop);
    at->addIncoming(next, loop);
    _ctx.builder->CreateCondBr(
        _ctx.builder->CreateICmpNE(more, llvm::ConstantInt::get(i64, 0)), loop, done);

    _ctx.set_insert_point(done);
    llvm::Value *start = next;
    llvm::Value *len = _ctx.builder->CreateSub(idx, start, "crash.n");

    write_pieces(
        headline.bytes, headline.size,
        message.bytes, message.size,
        file.bytes, file.size,
        _ctx.builder->CreateInBoundsGEP(buf_ty, buf, { zero, start }, "crash.digits"),
        len);
}

void AbortCodegen::flush_stdout()
{
    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    llvm::PointerType *opaque_ptr = llvm::cast<llvm::PointerType>(_ctx.opaque_ptr_type());

    _ctx.builder->CreateCall(
        _ctx.libc_callee("fflush", i32, { opaque_ptr }),
        { llvm::ConstantPointerNull::get(opaque_ptr) });
}

void AbortCodegen::write_stderr(llvm::Value *ptr, llvm::Value *len)
{
    // write(2, ...) rather than fprintf(stderr, ...): the `stderr` global is spelled differently
    // per platform (`__stderrp` on Darwin) and is not portably addressable from IR
    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();

    _ctx.builder->CreateCall(
        _ctx.libc_callee("write", i64, { i32, opaque_ptr, i64 }),
        { llvm::ConstantInt::get(i32, 2), ptr, len });
}

void AbortCodegen::call_thunk(
    const std::string &headline,
    llvm::Value *detail_ptr,
    llvm::Value *detail_len,
    const TokenReference &at
)
{
    llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);

    const std::string file = file_basename(at);
    const std::string line = std::to_string(at.line());

    _ctx.builder->CreateCall(get_or_create_abort_thunk(), {
        _ctx.builder->CreateGlobalStringPtr(headline, ""),
        llvm::ConstantInt::get(i64, headline.size()),
        detail_ptr,
        detail_len,
        _ctx.builder->CreateGlobalStringPtr(file, ""),
        llvm::ConstantInt::get(i64, file.size()),
        llvm::ConstantInt::get(i32, at.line()),
        _ctx.builder->CreateGlobalStringPtr(line, ""),
        llvm::ConstantInt::get(i64, line.size()),
    });
    _ctx.builder->CreateUnreachable();
}

void AbortCodegen::gen_abort(
    const std::string &headline,
    const std::string &detail,
    const TokenReference &at
)
{
    llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
    call_thunk(
        headline,
        _ctx.builder->CreateGlobalStringPtr(detail, ""),
        llvm::ConstantInt::get(i64, detail.size()),
        at);
}

void AbortCodegen::gen_abort_dynamic(
    const std::string &headline,
    llvm::Value *detail_ptr,
    llvm::Value *detail_len,
    const TokenReference &at
)
{
    call_thunk(headline, detail_ptr, detail_len, at);
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
    const TokenReference &at
)
{
    llvm::Function *fn = _ctx.builder->GetInsertBlock()->getParent();

    llvm::BasicBlock *abort_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "abort", fn);
    llvm::BasicBlock *ok_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "abort.ok", fn);

    _ctx.builder->CreateCondBr(condition, abort_block, ok_block);

    _ctx.set_insert_point(abort_block);
    gen_abort(headline, detail, at);

    _ctx.set_insert_point(ok_block);
}

llvm::Value *AbortCodegen::swap_hook(llvm::Value *fn)
{
    // the slot is reachable from every thread the moment a program can spawn one
    llvm::Value *old = _ctx.builder->CreateAtomicRMW(
        llvm::AtomicRMWInst::Xchg,
        hook_global(),
        fn,
        llvm::Align(8),
        llvm::AtomicOrdering::SequentiallyConsistent);
    old->setName("crash.prev");
    return old;
}

llvm::Value *AbortCodegen::take_hook()
{
    llvm::Type *opaque_ptr = _ctx.opaque_ptr_type();
    llvm::Value *old = _ctx.builder->CreateAtomicRMW(
        llvm::AtomicRMWInst::Xchg,
        hook_global(),
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(opaque_ptr)),
        llvm::Align(8),
        llvm::AtomicOrdering::SequentiallyConsistent);
    old->setName("crash.prev");
    return old;
}

void AbortCodegen::gen_default_hook(llvm::Value *info)
{
    emit_default_print_body(info);
}

std::string AbortCodegen::detail_of(const AST::FunctionCallExprNode &node) const
{
    if (node.decl == nullptr || !node.decl->is_builtin()) {
        return "";
    }

    const auto index = AST::builtin_message_index(
        AST::builtin_kind_for(node.decl->builtin.value()));

    if (!index.has_value() || node.arguments.size() <= *index) {
        return "";
    }

    return AST::literal_string_value(node.arguments[*index]).value_or("");
}

};
