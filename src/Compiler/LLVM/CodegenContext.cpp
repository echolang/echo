#include "Compiler/LLVM/CodegenContext.h"

#include <llvm/Support/raw_ostream.h>

namespace Compiler::LLVM
{
    CmpUnit *CodegenContext::main_cmp_unit()
    {
        auto it = cmp_unit_map.find(ECO_MAIN_MODULE_NAME);
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

    Compiler::InternalCompilerException CodegenContext::error(std::string message)
    {
        return Compiler::InternalCompilerException(message, current_file);
    }
}
