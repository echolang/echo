#include "AST/ASTContext.h"

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