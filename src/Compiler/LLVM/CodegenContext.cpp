#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/Codegen/DebugInfoCodegen.h"
#include "Compiler/LLVM/Codegen/TypeLowering.h"

#include "AST/ASTFile.h"
#include "AST/ASTMemberLookup.h"
#include "AST/FunctionDeclNode.h"

#include <fmt/core.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>

#include <vector>

namespace Compiler::LLVM
{
    llvm::Function *CodegenContext::llvm_function(const AST::FunctionDeclNode *decl)
    {
        // the callee symbol *in the current unit*, declared on demand. searching another unit's
        // table is silent UB: an llvm::Function belongs to one Module, compile_bundle then
        // destroys the foreign one, and the verifier does not check module membership
        if (decl == nullptr) {
            return nullptr;
        }

        auto funcid = current_cmp_unit->function_table.get_function_id(decl);

        if (llvm::Function *func = current_cmp_unit->function_table.get_llvm_function(funcid)) {
            return func;
        }

        return types->create_llvm_func_decl(decl, *current_cmp_unit);
    }

    void CodegenContext::emit_call(
        llvm::FunctionCallee callee,
        std::vector<llvm::Value *> &args,
        const ReturnAbi &abi
    )
    {
        // **an aggregate too big for registers comes back through storage this call site provides.**
        // `abi` is the same answer the signature asked, so a caller and its callee cannot disagree about
        // where the answer is. the slot is an ordinary entry alloca, which is what lets SROA promote it
        // into scalars once the callee is inlined
        //
        // **the slot's type comes from this unit's lowering of the ABI, never from the `sret` attribute
        // on the callee.** reading it off the attribute is the same answer right up until the modules are
        // merged: the JIT and `--optimize whole` both link every unit into one and `llvm::Linker` brings
        // each unit's own named struct types along, so the attribute can name the *other* unit's `%string`
        // while this unit allocates and reads its own

        if (abi.is_indirect()) {
            llvm::Value *slot = entry_alloca(abi.indirect_type, "call.sret");

            args.insert(args.begin(), slot);

            auto *call = builder->CreateCall(callee, args);

            // **the attribute goes on the call too, and forgetting it is a miscompile rather than a missed
            // optimization** - see the note on Compiler::LLVM::indirect_return_attributes. it decides which
            // register the hidden pointer travels in, and LLVM's fallback to the callee's own attributes stops
            // working the moment a merge leaves this unit's `%string` and the callee's as two types
            call->setAttributes(call->getAttributes().addParamAttributes(
                *llvm_context, 0,
                indirect_return_attributes(*llvm_context, abi, layout())));

            // the call answers `void`, so the value this expression produces is what the callee wrote
            value_stack.push(builder->CreateLoad(abi.indirect_type, slot, "call.result"));

            return;
        }

        auto *call = builder->CreateCall(callee, args);

        // a void call produces no value. pushing one anyway left a void-typed entry that no
        // parent ever pops, so a `foo();` statement quietly grew the stack
        if (!call->getType()->isVoidTy()) {
            value_stack.push(call);
        }
    }

    llvm::Value *CodegenContext::string_as_view(
        llvm::Value *value,
        const AST::ValueType &type,
        const char *prefix
    )
    {
        if (!core_types().is_string(type)) {
            return value;
        }

        AST::FunctionDeclNode *to_view = AST::find_implicit_conversion(
            type, core_types().string_view_type());

        if (to_view == nullptr) {
            // a hand-declared `#[core: string]` with no conversion: the stored window is the truth
            const AST::CoreStringLayout &layout = core_string_layout();

            return builder->CreateExtractValue(
                value, { static_cast<unsigned>(layout.window_index) }, fmt::format("{}window", prefix));
        }

        llvm::Type *string_ty = types->get_llvm_type(type, *current_cmp_unit);
        llvm::Value *self = entry_alloca(string_ty, fmt::format("{}slot", prefix));

        builder->CreateStore(value, self);

        llvm::Function *fn = llvm_function(to_view);
        std::vector<llvm::Value *> args = { self };

        emit_call(fn, args, types->return_abi_of(to_view, *current_cmp_unit));

        return pop();
    }

    CodegenContext::StringWindow CodegenContext::gen_string_window(
        llvm::Value *view,
        const char *prefix
    )
    {
        const AST::CoreStringLayout &layout = core_string_layout();

        return StringWindow{
            builder->CreateExtractValue(
                view, { static_cast<unsigned>(layout.bytes_index) }, fmt::format("{}bytes", prefix)),
            builder->CreateExtractValue(
                view, { static_cast<unsigned>(layout.size_index) }, fmt::format("{}size", prefix)),
        };
    }

    void CodegenContext::set_insert_point(llvm::BasicBlock *block)
    {
        builder->SetInsertPoint(block);

        // the location does not travel with the builder on its own - see the header. decided rather
        // than saved and restored per caller, because the statement seam owns which one is current and
        // a caller holding its own copy is a second answer to that
        if (debug_info != nullptr) {
            debug_info->relocate(block);
        }
    }

    void CodegenContext::set_insert_point(llvm::BasicBlock *block, llvm::BasicBlock::iterator point)
    {
        builder->SetInsertPoint(block, point);

        if (debug_info != nullptr) {
            debug_info->relocate(block);
        }
    }

    CmpUnit *CodegenContext::main_cmp_unit()
    {
        auto it = cmp_unit_map.find(entry_module_name);
        if (it == cmp_unit_map.end()) {
            return nullptr;
        }

        return it->second;
    }

    bool CodegenContext::file_is_entry(const AST::File &file) const
    {
        // **no target named one, so all of them are.** A program's `main` is the concatenation of every
        // file root of its entry module unless a `#[target:]` says which single file it is
        if (entry_file.empty()) {
            return true;
        }

        return file.get_path() == entry_file;
    }

    std::string CodegenContext::llvm_err_str()
    {
        std::string error;
        llvm::raw_string_ostream error_stream(error);
        llvm::errs().write(error_stream.str().data(), error_stream.str().size());
        return error;
    }

    std::string CodegenContext::function_context() const
    {
        if (current_function) {
            return fmt::format("in function '{}'", current_function->func_name());
        }
        return "at global scope";
    }

    Compiler::InternalCompilerException CodegenContext::error(std::string message)
    {
        return Compiler::InternalCompilerException(message, current_file);
    }
};
