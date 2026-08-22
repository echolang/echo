#include "Compiler/LLVM/Codegen/StaticStorageCodegen.h"

#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"

#include "AST/FunctionDeclNode.h"
#include "AST/StaticPropertyExprNode.h"
#include "AST/VarDeclNode.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

#include <fmt/core.h>

namespace Compiler::LLVM
{
    namespace
    {
        // the head of the teardown chain, and the walk over it. one symbol pair for the whole program,
        // `linkonce_odr` like everything else this subsystem emits
        constexpr const char *k_chain_head_symbol = "__eco_static_chain";
        constexpr const char *k_teardown_symbol = "__eco_static_teardown";
        constexpr const char *k_once_symbol = "__eco_static_once";

        // `{ ptr next, ptr fn }` - one node per static that owes a teardown, pushed by its init function
        // after the value is seated. **an intrusive node rather than a list the runtime allocates**,
        // because a teardown that had to allocate would be one more thing to get wrong at exit, and
        // because a zero-initialized global raises no ODR question at all
        llvm::StructType *chain_node_type(CodegenContext &ctx)
        {
            llvm::Type *ptr = ctx.opaque_ptr_type();
            return llvm::StructType::get(*ctx.llvm_context, { ptr, ptr });
        }
    }

    std::string StaticStorageCodegen::symbol_for(AST::StaticPropertyExprNode &node)
    {
        // the owner's mangled token carries its type arguments, so `Box<int32>` and `Box<float>` name
        // two globals with nothing here to arrange it. the index sits beside the name for the reason a
        // struct field's does: a rename is a different symbol, a reorder is not
        return fmt::format(
            "{}.s{}.{}",
            node.owner.get_complex_type()->mangled_token(),
            node.index,
            node.token_name.value());
    }

    llvm::Function *StaticStorageCodegen::declare_on_demand(AST::FunctionDeclNode *decl)
    {
        if (decl == nullptr) {
            return nullptr;
        }

        // look before creating: create_llvm_func_decl refuses a second declaration of one symbol, and
        // this runs once per *unit* that references the static rather than once per program
        auto id = _ctx.current_cmp_unit->function_table.get_function_id(decl);

        if (llvm::Function *existing = _ctx.current_cmp_unit->function_table.get_llvm_function(id)) {
            return existing;
        }

        return _ctx.types->create_llvm_func_decl(decl, *_ctx.current_cmp_unit);
    }

    llvm::GlobalVariable *StaticStorageCodegen::storage_for(
        AST::StaticPropertyExprNode &node,
        const std::string &symbol
    )
    {
        return _ctx.get_or_create_odr_global(
            symbol.c_str(), _ctx.types->get_llvm_type(node.result_type(), *_ctx.current_cmp_unit));
    }

    llvm::GlobalVariable *StaticStorageCodegen::guard_for(const std::string &symbol)
    {
        const std::string guard_symbol = symbol + ".guard";

        return _ctx.get_or_create_odr_global(
            guard_symbol.c_str(), llvm::Type::getInt64Ty(*_ctx.llvm_context));
    }

    llvm::Function *StaticStorageCodegen::once_helper()
    {
        if (auto *existing = _ctx.current_module()->getFunction(k_once_symbol)) {
            return existing;
        }

        llvm::Type *void_ty = llvm::Type::getVoidTy(*_ctx.llvm_context);
        llvm::Type *ptr = _ctx.opaque_ptr_type();
        llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);

        auto *fn = llvm::Function::Create(
            llvm::FunctionType::get(void_ty, { ptr, ptr }, false),
            llvm::GlobalValue::LinkOnceODRLinkage,
            k_once_symbol,
            _ctx.current_module());
        fn->getArg(0)->setName("guard");
        fn->getArg(1)->setName("body");

        llvm::IRBuilderBase::InsertPointGuard restore_point(*_ctx.builder);
        _ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());

        auto *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", fn);
        auto *loop = llvm::BasicBlock::Create(*_ctx.llvm_context, "loop", fn);
        auto *try_cas = llvm::BasicBlock::Create(*_ctx.llvm_context, "try", fn);
        auto *run = llvm::BasicBlock::Create(*_ctx.llvm_context, "run", fn);
        auto *wait = llvm::BasicBlock::Create(*_ctx.llvm_context, "wait", fn);
        auto *done = llvm::BasicBlock::Create(*_ctx.llvm_context, "done", fn);

        llvm::Value *guard = fn->getArg(0);
        llvm::Value *body = fn->getArg(1);
        llvm::Constant *done_tok = llvm::ConstantInt::get(i64, static_cast<uint64_t>(-1));

        _ctx.builder->SetInsertPoint(entry);
        // i64, not ptr: stdlib declares `pthread_self` as returning usize, and two units that
        // disagree about that symbol's type emit two __eco_static_once bodies the ODR check
        // refuses. the bits in the return register are the thread id either way
        llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
        llvm::Value *self = nullptr;

        if (_ctx.targeting_windows()) {
            // DWORD GetCurrentThreadId(void) - kernel32, already on the link line
            llvm::Value *tid = _ctx.builder->CreateCall(
                _ctx.libc_callee("GetCurrentThreadId", i32, {}), {}, "tid");
            self = _ctx.builder->CreateZExt(tid, i64, "self");
        }
        else {
            self = _ctx.builder->CreateCall(
                _ctx.libc_callee("pthread_self", i64, {}), {}, "self");
            _ctx.needs_pthread = true;
        }
        // 0 is uninitialized and ~0 is done. a tid that collides with either would skip
        // the initializer or look finished. 1 is neither sentinel
        llvm::Value *one = llvm::ConstantInt::get(i64, 1);
        llvm::Value *tid_is_sentinel = _ctx.builder->CreateOr(
            _ctx.builder->CreateICmpEQ(self, llvm::ConstantInt::get(i64, 0), "tid.zero"),
            _ctx.builder->CreateICmpEQ(self, done_tok, "tid.done"),
            "tid.sentinel");
        llvm::Value *token = _ctx.builder->CreateSelect(tid_is_sentinel, one, self, "token");
        _ctx.builder->CreateBr(loop);

        _ctx.builder->SetInsertPoint(loop);
        llvm::LoadInst *seen = _ctx.builder->CreateLoad(i64, guard, "seen");
        seen->setAtomic(llvm::AtomicOrdering::Acquire);
        seen->setAlignment(llvm::Align(8));
        llvm::Value *is_done = _ctx.builder->CreateICmpEQ(seen, done_tok, "is.done");
        llvm::Value *is_self = _ctx.builder->CreateICmpEQ(seen, token, "is.self");
        llvm::Value *mine_or_done = _ctx.builder->CreateOr(is_done, is_self, "mine.or.done");
        llvm::Value *in_flight = _ctx.builder->CreateICmpNE(
            seen, llvm::ConstantInt::get(i64, 0), "in.flight");
        _ctx.builder->CreateCondBr(mine_or_done, done, try_cas);

        _ctx.builder->SetInsertPoint(try_cas);
        _ctx.builder->CreateCondBr(in_flight, wait, run);

        // CAS 0 → token. the run block is only reached when seen was 0, so a failed CAS
        // is another thread winning the race - loop and look again
        _ctx.builder->SetInsertPoint(run);
        llvm::AtomicCmpXchgInst *cas = _ctx.builder->CreateAtomicCmpXchg(
            guard,
            llvm::ConstantInt::get(i64, 0),
            token,
            llvm::Align(8),
            llvm::AtomicOrdering::Acquire,
            llvm::AtomicOrdering::Monotonic);
        llvm::Value *ok = _ctx.builder->CreateExtractValue(cas, 1, "won");
        auto *call_body = llvm::BasicBlock::Create(*_ctx.llvm_context, "call.body", fn);
        _ctx.builder->CreateCondBr(ok, call_body, loop);

        _ctx.builder->SetInsertPoint(call_body);
        auto *body_ty = llvm::FunctionType::get(void_ty, false);
        _ctx.builder->CreateCall(body_ty, body);
        llvm::StoreInst *finish = _ctx.builder->CreateStore(done_tok, guard);
        finish->setAtomic(llvm::AtomicOrdering::Release);
        finish->setAlignment(llvm::Align(8));
        _ctx.builder->CreateBr(done);

        _ctx.builder->SetInsertPoint(wait);
        if (_ctx.targeting_windows()) {
            _ctx.builder->CreateCall(_ctx.libc_callee("SwitchToThread", i32, {}));
        }
        else {
            _ctx.builder->CreateCall(_ctx.libc_callee("sched_yield", i32, {}));
        }
        _ctx.builder->CreateBr(loop);

        _ctx.builder->SetInsertPoint(done);
        _ctx.builder->CreateRetVoid();

        return fn;
    }

    llvm::Function *StaticStorageCodegen::init_for(
        AST::StaticPropertyExprNode &node,
        const std::string &symbol
    )
    {
        const std::string init_symbol = symbol + ".init";

        if (auto *existing = _ctx.current_module()->getFunction(init_symbol)) {
            return existing;
        }

        llvm::Type *void_ty = llvm::Type::getVoidTy(*_ctx.llvm_context);
        llvm::Type *i64 = llvm::Type::getInt64Ty(*_ctx.llvm_context);
        llvm::Type *ptr = _ctx.opaque_ptr_type();

        auto *fn = llvm::Function::Create(
            llvm::FunctionType::get(void_ty, false),
            llvm::GlobalValue::LinkOnceODRLinkage,
            init_symbol,
            _ctx.current_module());

        const std::string body_symbol = symbol + ".init.body";
        auto *body = llvm::Function::Create(
            llvm::FunctionType::get(void_ty, false),
            llvm::GlobalValue::LinkOnceODRLinkage,
            body_symbol,
            _ctx.current_module());

        llvm::IRBuilderBase::InsertPointGuard restore_point(*_ctx.builder);
        _ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());

        llvm::GlobalVariable *guard = guard_for(symbol);
        llvm::Constant *done_tok = llvm::ConstantInt::get(i64, static_cast<uint64_t>(-1));

        // **the fast path stays one acquire load.** inlining the owner-token dance everywhere is
        // fifteen blocks per static; routing every access through the helper puts a call in front
        // of every static read. this keeps a static read at one acquire load - the same
        // instruction a plain load lowers to on both architectures
        auto *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", fn);
        auto *slow = llvm::BasicBlock::Create(*_ctx.llvm_context, "slow", fn);
        auto *done = llvm::BasicBlock::Create(*_ctx.llvm_context, "done", fn);

        _ctx.builder->SetInsertPoint(entry);
        llvm::LoadInst *seen = _ctx.builder->CreateLoad(i64, guard, "guard");
        seen->setAtomic(llvm::AtomicOrdering::Acquire);
        seen->setAlignment(llvm::Align(8));
        _ctx.builder->CreateCondBr(
            _ctx.builder->CreateICmpEQ(seen, done_tok, "initialized"),
            done,
            slow);

        _ctx.builder->SetInsertPoint(slow);
        _ctx.builder->CreateCall(once_helper(), { guard, body });
        _ctx.builder->CreateBr(done);

        _ctx.builder->SetInsertPoint(done);
        _ctx.builder->CreateRetVoid();

        // `<sym>.init.body` holds what init_block held: the seat, then the Treiber push
        auto *body_entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", body);
        _ctx.builder->SetInsertPoint(body_entry);

        if (llvm::Function *seat = declare_on_demand(node.init)) {
            _ctx.builder->CreateCall(seat);
        }

        if (llvm::Function *end = declare_on_demand(node.deinit)) {
            llvm::StructType *node_type = chain_node_type(_ctx);
            const std::string link_symbol = symbol + ".link";

            llvm::GlobalVariable *link = _ctx.get_or_create_odr_global(link_symbol.c_str(), node_type);
            llvm::GlobalVariable *head = _ctx.get_or_create_odr_global(k_chain_head_symbol, ptr);

            llvm::Value *next_slot = _ctx.builder->CreateStructGEP(node_type, link, 0, "link.next");
            llvm::Value *fn_slot = _ctx.builder->CreateStructGEP(node_type, link, 1, "link.fn");

            // store the fn slot plainly, then a release/monotonic CAS loop publishing the head
            _ctx.builder->CreateStore(end, fn_slot);

            auto *push = llvm::BasicBlock::Create(*_ctx.llvm_context, "push", body);
            auto *pushed = llvm::BasicBlock::Create(*_ctx.llvm_context, "pushed", body);
            _ctx.builder->CreateBr(push);

            _ctx.builder->SetInsertPoint(push);
            llvm::LoadInst *cur = _ctx.builder->CreateLoad(ptr, head, "chain");
            cur->setAtomic(llvm::AtomicOrdering::Monotonic);
            cur->setAlignment(llvm::Align(8));
            _ctx.builder->CreateStore(cur, next_slot);
            llvm::AtomicCmpXchgInst *cas = _ctx.builder->CreateAtomicCmpXchg(
                head,
                cur,
                link,
                llvm::Align(8),
                llvm::AtomicOrdering::Release,
                llvm::AtomicOrdering::Monotonic);
            llvm::Value *ok = _ctx.builder->CreateExtractValue(cas, 1, "published");
            _ctx.builder->CreateCondBr(ok, pushed, push);

            _ctx.builder->SetInsertPoint(pushed);
        }

        _ctx.builder->CreateRetVoid();

        return fn;
    }

    llvm::Value *StaticStorageCodegen::gen_address(AST::StaticPropertyExprNode &node)
    {
        // taken once and handed down: it is what the storage, the guard, the init function and the
        // chain link are all named from, and deriving it walks the owner's mangled token every time
        const std::string symbol = symbol_for(node);

        llvm::GlobalVariable *storage = storage_for(node, symbol);

        // **one call, at the one place every access already goes through.** emitted here rather than
        // inline so gen_lvalue stays a pure address producer - see the header for why that matters
        _ctx.builder->CreateCall(init_for(node, symbol));

        return storage;
    }

    void StaticStorageCodegen::gen_teardown()
    {
        llvm::Type *ptr = _ctx.opaque_ptr_type();

        // nothing pushed a node, so there is nothing to walk. asked of the *module* rather than of a
        // count kept here, so a unit that references no static emits no walk at all
        auto *head = _ctx.current_module()->getGlobalVariable(k_chain_head_symbol, true);

        if (head == nullptr) {
            return;
        }

        llvm::StructType *node_type = chain_node_type(_ctx);

        auto *fn = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(*_ctx.llvm_context), false),
            llvm::GlobalValue::LinkOnceODRLinkage,
            k_teardown_symbol,
            _ctx.current_module());

        // scoped, so the builder is back in the caller's block - `main`'s epilogue - before the call to
        // this function is emitted below. the same restore init_for takes, and for the same reason
        {
            llvm::IRBuilderBase::InsertPointGuard restore_point(*_ctx.builder);

            _ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());

            auto *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", fn);
            auto *loop = llvm::BasicBlock::Create(*_ctx.llvm_context, "loop", fn);
            auto *body = llvm::BasicBlock::Create(*_ctx.llvm_context, "body", fn);
            auto *done = llvm::BasicBlock::Create(*_ctx.llvm_context, "done", fn);

            _ctx.builder->SetInsertPoint(entry);
            llvm::LoadInst *first = _ctx.builder->CreateLoad(ptr, head, "first");
            first->setAtomic(llvm::AtomicOrdering::Acquire);
            first->setAlignment(llvm::Align(8));
            _ctx.builder->CreateBr(loop);

            _ctx.builder->SetInsertPoint(loop);
            llvm::PHINode *current = _ctx.builder->CreatePHI(ptr, 2, "node");
            current->addIncoming(first, entry);
            _ctx.builder->CreateCondBr(
                _ctx.builder->CreateICmpEQ(current, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)), "at.end"),
                done,
                body);

            _ctx.builder->SetInsertPoint(body);
            llvm::Value *next = _ctx.builder->CreateLoad(
                ptr, _ctx.builder->CreateStructGEP(node_type, current, 0, "next.slot"), "next");
            llvm::Value *end = _ctx.builder->CreateLoad(
                ptr, _ctx.builder->CreateStructGEP(node_type, current, 1, "fn.slot"), "fn");

            _ctx.builder->CreateCall(
                llvm::FunctionType::get(llvm::Type::getVoidTy(*_ctx.llvm_context), false), end);

            _ctx.builder->CreateBr(loop);
            current->addIncoming(next, body);

            _ctx.builder->SetInsertPoint(done);
            _ctx.builder->CreateRetVoid();
        }

        // and the call, in whichever block the caller left live - `main`'s epilogue
        _ctx.builder->CreateCall(fn);
    }
};
