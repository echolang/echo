#include "Compiler/LLVM/Codegen/StaticStorageCodegen.h"

#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"

#include "AST/FunctionDeclNode.h"
#include "AST/StaticPropertyExprNode.h"
#include "AST/VarDeclNode.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

#include <fmt/core.h>

namespace Compiler::LLVM
{
    namespace
    {
        // the head of the teardown chain, and the walk over it. one symbol pair for the whole program,
        // `linkonce_odr` like everything else this subsystem emits
        constexpr const char *k_chain_head_symbol = "__eco_static_chain";
        constexpr const char *k_teardown_symbol = "__eco_static_teardown";

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
            guard_symbol.c_str(), llvm::Type::getInt8Ty(*_ctx.llvm_context));
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

        llvm::Type *i8 = llvm::Type::getInt8Ty(*_ctx.llvm_context);
        llvm::Type *ptr = _ctx.opaque_ptr_type();

        auto *fn = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(*_ctx.llvm_context), false),
            llvm::GlobalValue::LinkOnceODRLinkage,
            init_symbol,
            _ctx.current_module());

        auto *entry = llvm::BasicBlock::Create(*_ctx.llvm_context, "entry", fn);
        auto *init_block = llvm::BasicBlock::Create(*_ctx.llvm_context, "init", fn);
        auto *done = llvm::BasicBlock::Create(*_ctx.llvm_context, "done", fn);

        // the builder is mid-statement in whatever body asked for this address, so everything below has
        // to be put back exactly as it was found - this function is emitted *beside* that one. the guard
        // restores the block, the insert point and the debug location together, and on every exit
        llvm::IRBuilderBase::InsertPointGuard restore_point(*_ctx.builder);

        // no debug location on any of this: it is compiler-emitted runtime with no line of its own, and
        // a location inherited from the statement that happened to reference it first is exactly the
        // ambient state a linkonce_odr body may not depend on
        _ctx.builder->SetCurrentDebugLocation(llvm::DebugLoc());

        llvm::GlobalVariable *guard = guard_for(symbol);

        _ctx.builder->SetInsertPoint(entry);
        llvm::Value *seen = _ctx.builder->CreateLoad(i8, guard, "guard");
        _ctx.builder->CreateCondBr(
            _ctx.builder->CreateICmpNE(seen, llvm::ConstantInt::get(i8, 0), "initialized"),
            done,
            init_block);

        _ctx.builder->SetInsertPoint(init_block);

        // **the guard is set before the body runs, and that is the recursion answer.** a static whose
        // initializer reads itself re-enters here, finds the guard set and returns, so the read sees the
        // zero the global was created with - a defined value rather than undefined behaviour or a
        // deadlock. it is the specified answer, not an accident of ordering
        _ctx.builder->CreateStore(llvm::ConstantInt::get(i8, 1), guard);

        // the initializer, which AST::OwnershipPass synthesized as an ordinary function: its body is
        // `Type::$x = <what was written>;` in a real scope, so every ownership rule, every copy and
        // every temporary was decided by the passes that already know how, with nothing here to repeat
        if (llvm::Function *seat = declare_on_demand(node.init)) {
            _ctx.builder->CreateCall(seat);
        }

        // and the teardown, only for a static that owes one. the node is pushed *after* the value is
        // seated, so the chain's LIFO order is reverse-of-initialization for free - including across
        // statics whose initializers named each other
        if (llvm::Function *end = declare_on_demand(node.deinit)) {
            llvm::StructType *node_type = chain_node_type(_ctx);
            const std::string link_symbol = symbol + ".link";

            llvm::GlobalVariable *link = _ctx.get_or_create_odr_global(link_symbol.c_str(), node_type);
            llvm::GlobalVariable *head = _ctx.get_or_create_odr_global(k_chain_head_symbol, ptr);

            llvm::Value *next_slot = _ctx.builder->CreateStructGEP(node_type, link, 0, "link.next");
            llvm::Value *fn_slot = _ctx.builder->CreateStructGEP(node_type, link, 1, "link.fn");

            _ctx.builder->CreateStore(_ctx.builder->CreateLoad(ptr, head, "chain"), next_slot);
            _ctx.builder->CreateStore(end, fn_slot);
            _ctx.builder->CreateStore(link, head);
        }

        _ctx.builder->CreateBr(done);

        _ctx.builder->SetInsertPoint(done);
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
            llvm::Value *first = _ctx.builder->CreateLoad(ptr, head, "first");
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
