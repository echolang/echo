#include "AST/ASTContext.h"

#include "AST/ASTTypeParam.h"

const AST::TypeParamDecl *AST::Context::find_type_param(const std::string &name) const
{
    // innermost scope first, so an inner parameter shadows an outer one of the same name
    for (auto scope = type_param_scopes.rbegin(); scope != type_param_scopes.rend(); ++scope) {
        for (const auto *param : *scope) {
            if (param->name == name) {
                return param;
            }
        }
    }

    return nullptr;
}

void AST::Context::pop_scope()
{
    // we must have an active scope to pop
    assert(scope_ptr != nullptr);
    scope_ptr = scope_ptr->parent_ptr;
}

void AST::Context::push_scope(ScopeNode &scope)
{
    // if we have an active scope, add the new scope as a child
    if (scope_ptr != nullptr) {
        scope.parent_ptr = scope_ptr;
    }

    // update the current scope
    scope_ptr = &scope;
}