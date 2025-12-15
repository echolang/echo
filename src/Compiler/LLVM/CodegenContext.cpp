#include "Compiler/LLVM/CodegenContext.h"
#include "Compiler/LLVM/Codegen/DebugInfoCodegen.h"

#include "AST/ASTFile.h"
#include "AST/FunctionDeclNode.h"

#include <fmt/core.h>
#include <llvm/Support/raw_ostream.h>

namespace Compiler::LLVM
{
    CodegenContext::StringWindow CodegenContext::gen_string_window(
        llvm::Value *value, const AST::ValueType &type, const char *prefix)
    {
        const AST::CoreStringLayout &layout = core_string_layout();

        llvm::Value *window = core_types().is_string(type)
            ? builder->CreateExtractValue(
                  value, { static_cast<unsigned>(layout.window_index) }, fmt::format("{}window", prefix))
            : value;

        return StringWindow{
            builder->CreateExtractValue(
                window, { static_cast<unsigned>(layout.bytes_index) }, fmt::format("{}bytes", prefix)),
            builder->CreateExtractValue(
                window, { static_cast<unsigned>(layout.size_index) }, fmt::format("{}size", prefix)),
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

    AST::File *CodegenContext::file_of_token(const TokenReference &token) const
    {
        // one module owns the collection this token is in, and every other answers null for it - so the
        // first non-null is the answer and there is nothing to disambiguate
        for (AST::Module *module : token_modules) {
            if (const AST::File *file = module->file_of(token)) {
                return const_cast<AST::File *>(file);
            }
        }

        return nullptr;
    }

    bool CodegenContext::is_virtual_token(const TokenReference &token) const
    {
        return file_of_token(token) == nullptr;
    }

    CmpUnit *CodegenContext::main_cmp_unit()
    {
        auto it = cmp_unit_map.find(entry_module_name);
        if (it == cmp_unit_map.end()) {
            return nullptr;
        }

        return it->second;
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

    std::string CodegenContext::current_file_name() const
    {
        if (current_file == nullptr) {
            return "<unknown>";
        }
        return current_file->get_path().filename().string();
    }

    Compiler::InternalCompilerException CodegenContext::error(std::string message)
    {
        return Compiler::InternalCompilerException(message, current_file);
    }
};
