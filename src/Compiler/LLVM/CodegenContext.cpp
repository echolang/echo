#include "Compiler/LLVM/CodegenContext.h"

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
