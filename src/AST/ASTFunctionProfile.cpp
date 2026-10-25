#include "AST/ASTFunctionProfile.h"

#include "AST/ASTFunctionEmission.h"
#include "AST/ASTModule.h"
#include "AST/FunctionDeclNode.h"

bool AST::profile_instruments(const FunctionDeclNode *decl, bool include_stdlib)
{
    if (decl == nullptr || decl->is_implicitly_generated) {
        return false;
    }

    switch (function_emission_kind(decl)) {
    case FunctionEmission::t_no_symbol:
    case FunctionEmission::t_extern_symbol:
    case FunctionEmission::t_intrinsic:
        return false;
    default:
        break;
    }

    if (!include_stdlib
        && decl->declared_in.module != nullptr
        && decl->declared_in.module->compiler_supplied) {
        return false;
    }

    return true;
}
