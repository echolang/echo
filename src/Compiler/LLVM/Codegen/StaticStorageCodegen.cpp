#include "Compiler/LLVM/Codegen/StaticStorageCodegen.h"

#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/Codegen/AbortCodegen.h"
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

        // ~0 is done (fast path). ~1 is torn down: the slow path sees it, the fast path
        // does not pay a second compare. 0 is uninitialized, 1 is a tid that collided with a sentinel
        constexpr uint64_t k_guard_done = static_cast<uint64_t>(-1);
        constexpr uint64_t k_guard_dead = static_cast<uint64_t>(-2);

        void abort_if_guard_dead(CodegenContext &ctx, llvm::Value *seen, llvm::Value *dead_tok)
        {
            ctx.abort->gen_abort_if(
                ctx.builder->CreateICmpEQ(seen, dead_tok, "is.dead"),
                "fatal error",
                "a static was read after it was torn down",
                "static_dead");
        }

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
        llvm::Constant *done_tok = llvm::ConstantInt::get(i64, k_guard_done);
        llvm::Constant *dead_tok = llvm::ConstantInt::get(i64, k_guard_dead);
        llvm::Constant *one = llvm::ConstantInt::get(i64, 1);
        auto *body_ty = llvm::FunctionType::get(void_ty, false);

        auto *fn = llvm::Function::Create(
            llvm::FunctionType::get(void_ty, { ptr, ptr }, false),
            llvm::GlobalValue::LinkOnceODRLinkage,
            k_once_symbol,
            _ctx.current_module());
        fn->getArg(0)->setName("guard");
        fn->getArg(1)->setName("body");

        llvm::IRBuilderBase::InsertPointGuard restore_point(*_ctx.builder);
        _ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());

        llvm::Value *guard = fn->getArg(0);
        llvm::Value *body = fn->getArg(1);

        if (!_ctx.options.codegen.has_os_threads()) {
            auto *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", fn);
            auto *run = llvm::BasicBlock::Create(*_ctx.llvm_context, "run", fn);
            auto *done = llvm::BasicBlock::Create(*_ctx.llvm_context, "done", fn);

            _ctx.builder->SetInsertPoint(entry);
            llvm::LoadInst *seen = _ctx.builder->CreateLoad(i64, guard, "seen");
            seen->setAlignment(_ctx.layout().getABITypeAlign(i64));
            abort_if_guard_dead(_ctx, seen, dead_tok);
            llvm::Value *is_done = _ctx.builder->CreateICmpEQ(seen, done_tok, "is.done");
            llvm::Value *in_flight = _ctx.builder->CreateICmpNE(
                seen, llvm::ConstantInt::get(i64, 0), "in.flight");
            llvm::Value *skip = _ctx.builder->CreateOr(is_done, in_flight, "skip");
            _ctx.builder->CreateCondBr(skip, done, run);

            _ctx.builder->SetInsertPoint(run);
            llvm::StoreInst *claim = _ctx.builder->CreateStore(one, guard);
            claim->setAlignment(_ctx.layout().getABITypeAlign(i64));
            _ctx.builder->CreateCall(body_ty, body);
            llvm::StoreInst *finish = _ctx.builder->CreateStore(done_tok, guard);
            finish->setAlignment(_ctx.layout().getABITypeAlign(i64));
            _ctx.builder->CreateBr(done);

            _ctx.builder->SetInsertPoint(done);
            _ctx.builder->CreateRetVoid();
            return fn;
        }

        auto *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", fn);
        auto *loop = llvm::BasicBlock::Create(*_ctx.llvm_context, "loop", fn);
        auto *try_cas = llvm::BasicBlock::Create(*_ctx.llvm_context, "try", fn);
        auto *run = llvm::BasicBlock::Create(*_ctx.llvm_context, "run", fn);
        auto *wait = llvm::BasicBlock::Create(*_ctx.llvm_context, "wait", fn);
        auto *done = llvm::BasicBlock::Create(*_ctx.llvm_context, "done", fn);

        _ctx.builder->SetInsertPoint(entry);
        // i64, not ptr: stdlib declares `pthread_self` as returning usize, and two units that
        // disagree about that symbol's type emit two __eco_static_once bodies the ODR check
        // refuses. the bits in the return register are the thread id either way
        llvm::Type *i32 = llvm::Type::getInt32Ty(*_ctx.llvm_context);
        llvm::Value *self = nullptr;

        if (_ctx.targeting_windows()) {
            // DWORD GetCurrentThreadId(void) - kernel32, already on the link line
            llvm::Value *tid = _ctx.builder->CreateCall(
                _ctx.libc_callee(Compiler::RuntimeSymbol::t_get_current_thread_id, i32, {}), {}, "tid");
            self = _ctx.builder->CreateZExt(tid, i64, "self");
        }
        else {
            self = _ctx.builder->CreateCall(
                _ctx.libc_callee(Compiler::RuntimeSymbol::t_pthread_self, i64, {}), {}, "self");
            _ctx.needs_pthread = true;
        }
        // 0 is uninitialized, ~0 is done, ~1 is torn down. a tid that collides with any
        // would skip the initializer, look finished, or abort as dead. 1 is none of those
        llvm::Value *tid_is_sentinel = _ctx.builder->CreateOr(
            _ctx.builder->CreateOr(
                _ctx.builder->CreateICmpEQ(self, llvm::ConstantInt::get(i64, 0), "tid.zero"),
                _ctx.builder->CreateICmpEQ(self, done_tok, "tid.done"),
                "tid.zero.or.done"),
            _ctx.builder->CreateICmpEQ(self, dead_tok, "tid.dead"),
            "tid.sentinel");
        llvm::Value *token = _ctx.builder->CreateSelect(tid_is_sentinel, one, self, "token");
        _ctx.builder->CreateBr(loop);

        _ctx.builder->SetInsertPoint(loop);
        llvm::LoadInst *seen = _ctx.builder->CreateLoad(i64, guard, "seen");
        seen->setAtomic(llvm::AtomicOrdering::Acquire);
        seen->setAlignment(_ctx.layout().getABITypeAlign(i64));
        abort_if_guard_dead(_ctx, seen, dead_tok);
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
            _ctx.layout().getABITypeAlign(i64),
            llvm::AtomicOrdering::Acquire,
            llvm::AtomicOrdering::Monotonic);
        llvm::Value *ok = _ctx.builder->CreateExtractValue(cas, 1, "won");
        auto *call_body = llvm::BasicBlock::Create(*_ctx.llvm_context, "call.body", fn);
        _ctx.builder->CreateCondBr(ok, call_body, loop);

        _ctx.builder->SetInsertPoint(call_body);
        _ctx.builder->CreateCall(body_ty, body);
        llvm::StoreInst *finish = _ctx.builder->CreateStore(done_tok, guard);
        finish->setAtomic(llvm::AtomicOrdering::Release);
        finish->setAlignment(_ctx.layout().getABITypeAlign(i64));
        _ctx.builder->CreateBr(done);

        _ctx.builder->SetInsertPoint(wait);
        if (_ctx.targeting_windows()) {
            _ctx.builder->CreateCall(_ctx.libc_callee(Compiler::RuntimeSymbol::t_switch_to_thread, i32, {}));
        }
        else {
            _ctx.builder->CreateCall(_ctx.libc_callee(Compiler::RuntimeSymbol::t_sched_yield, i32, {}));
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
        llvm::Constant *done_tok = llvm::ConstantInt::get(i64, k_guard_done);

        // **the fast path stays one load.** inlining the owner-token dance everywhere is
        // fifteen blocks per static; routing every access through the helper puts a call in front
        // of every static read. threaded rows acquire; a row without OS threads is a plain load
        auto *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", fn);
        auto *slow = llvm::BasicBlock::Create(*_ctx.llvm_context, "slow", fn);
        auto *done = llvm::BasicBlock::Create(*_ctx.llvm_context, "done", fn);
        const bool threaded = _ctx.options.codegen.has_os_threads();

        _ctx.builder->SetInsertPoint(entry);
        llvm::LoadInst *seen = _ctx.builder->CreateLoad(i64, guard, "guard");
        if (threaded) {
            seen->setAtomic(llvm::AtomicOrdering::Acquire);
        }
        seen->setAlignment(_ctx.layout().getABITypeAlign(i64));
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

            // store a fini that runs deinit, zeros storage, then marks the guard dead so a
            // later load cannot treat the corpse as initialized
            const std::string fini_symbol = symbol + ".fini";
            llvm::Function *fini = _ctx.current_module()->getFunction(fini_symbol);
            if (fini == nullptr) {
                fini = llvm::Function::Create(
                    llvm::FunctionType::get(void_ty, false),
                    llvm::GlobalValue::LinkOnceODRLinkage,
                    fini_symbol,
                    _ctx.current_module());

                llvm::IRBuilderBase::InsertPointGuard fini_point(*_ctx.builder);
                _ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());
                auto *fini_entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", fini);
                _ctx.builder->SetInsertPoint(fini_entry);
                _ctx.builder->CreateCall(end);
                llvm::GlobalVariable *storage = storage_for(node, symbol);
                _ctx.builder->CreateStore(
                    llvm::Constant::getNullValue(storage->getValueType()), storage);
                llvm::StoreInst *dead = _ctx.builder->CreateStore(
                    llvm::ConstantInt::get(i64, k_guard_dead), guard);
                if (threaded) {
                    dead->setAtomic(llvm::AtomicOrdering::Release);
                }
                dead->setAlignment(_ctx.layout().getABITypeAlign(i64));
                _ctx.builder->CreateRetVoid();
            }

            _ctx.builder->CreateStore(fini, fn_slot);

            if (!threaded) {
                llvm::LoadInst *cur = _ctx.builder->CreateLoad(ptr, head, "chain");
                cur->setAlignment(_ctx.layout().getABITypeAlign(ptr));
                _ctx.builder->CreateStore(cur, next_slot);
                _ctx.builder->CreateStore(link, head);
            }
            else {
                auto *push = llvm::BasicBlock::Create(*_ctx.llvm_context, "push", body);
                auto *pushed = llvm::BasicBlock::Create(*_ctx.llvm_context, "pushed", body);
                _ctx.builder->CreateBr(push);

                _ctx.builder->SetInsertPoint(push);
                llvm::LoadInst *cur = _ctx.builder->CreateLoad(ptr, head, "chain");
                cur->setAtomic(llvm::AtomicOrdering::Monotonic);
                cur->setAlignment(_ctx.layout().getABITypeAlign(ptr));
                _ctx.builder->CreateStore(cur, next_slot);
                llvm::AtomicCmpXchgInst *cas = _ctx.builder->CreateAtomicCmpXchg(
                    head,
                    cur,
                    link,
                    _ctx.layout().getABITypeAlign(ptr),
                    llvm::AtomicOrdering::Release,
                    llvm::AtomicOrdering::Monotonic);
                llvm::Value *ok = _ctx.builder->CreateExtractValue(cas, 1, "published");
                _ctx.builder->CreateCondBr(ok, pushed, push);

                _ctx.builder->SetInsertPoint(pushed);
            }
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

    llvm::Function *StaticStorageCodegen::ensure_teardown_walker(bool create_head)
    {
        llvm::Type *ptr = _ctx.opaque_ptr_type();

        llvm::GlobalVariable *head = create_head
            ? _ctx.get_or_create_odr_global(k_chain_head_symbol, ptr)
            : _ctx.current_module()->getGlobalVariable(k_chain_head_symbol, true);

        // nothing pushed a node, so there is nothing to walk. asked of the *module*
        // rather than of a count kept here, so a unit that references no static
        // emits no walk at all - unless a lib fini asked for the head, in which
        // case a dependency's push still has somewhere to be found
        if (head == nullptr) {
            return nullptr;
        }

        if (llvm::Function *existing = _ctx.current_module()->getFunction(k_teardown_symbol)) {
            return existing;
        }

        llvm::StructType *node_type = chain_node_type(_ctx);

        auto *fn = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(*_ctx.llvm_context), false),
            llvm::GlobalValue::LinkOnceODRLinkage,
            k_teardown_symbol,
            _ctx.current_module());

        llvm::IRBuilderBase::InsertPointGuard restore_point(*_ctx.builder);

        _ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());

        auto *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", fn);
        auto *loop = llvm::BasicBlock::Create(*_ctx.llvm_context, "loop", fn);
        auto *body = llvm::BasicBlock::Create(*_ctx.llvm_context, "body", fn);
        auto *done = llvm::BasicBlock::Create(*_ctx.llvm_context, "done", fn);

        _ctx.builder->SetInsertPoint(entry);
        llvm::LoadInst *first = _ctx.builder->CreateLoad(ptr, head, "first");
        if (_ctx.options.codegen.has_os_threads()) {
            first->setAtomic(llvm::AtomicOrdering::Acquire);
        }
        first->setAlignment(_ctx.layout().getABITypeAlign(ptr));
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

        return fn;
    }

    void StaticStorageCodegen::gen_teardown()
    {
        llvm::Function *fn = ensure_teardown_walker(false);

        if (fn == nullptr) {
            return;
        }

        // the call, in whichever block the caller left live - `main`'s epilogue
        _ctx.builder->CreateCall(fn);
    }
};
