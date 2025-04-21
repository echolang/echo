#include "AST/ASTContext.h"

#include "AST/ASTTypeParam.h"
#include "AST/FunctionDeclNode.h"

AST::LexicalScope::LexicalScope(
    AST::Context &context,
    AST::NamespaceManager &namespaces,
    const std::optional<TokenReference> &block_token) :
    context(context), previous(context.current_namespace)
{
    if (!block_token.has_value()) {
        return;
    }

    // what a block's lexical namespace calls itself in a diagnostic: the enclosing function's name, so
    // a block-local declaration reports as `outer::helper(int32)`. empty at file scope, where there is
    // no function to name and the plain `helper(int32)` is what the user would expect to read
    const std::string display_name =
        context.current_function_ptr != nullptr ? context.current_function_ptr->func_name() : "";

    context.current_namespace = &namespaces.retrieve_lexical(
        *context.current_namespace, make_declaration_site(block_token.value()), display_name);
}

AST::MemberTypeScope::MemberTypeScope(
    AST::Context &context,
    AST::NamespaceManager &namespaces,
    const AST::ComplexType &owner) :
    context(context), previous(context.current_namespace)
{
    // an anonymous owner has no path to hang a child off; a closure environment is the only one, and it
    // declares no nested types. leaving the namespace alone is the safe answer either way
    if (!owner.name.has_value()) {
        return;
    }

    // the owner's own path plus its name. path_segments() rather than mangling_segments() because this
    // namespace is one a user writes - `A::Inner(1)` - and a type's namespace is never lexical, which is
    // the only place the two views differ
    std::vector<std::string> parts =
        owner.ast_namespace != nullptr ? owner.ast_namespace->path_segments() : std::vector<std::string>{};

    parts.push_back(owner.name.value());

    context.current_namespace = &namespaces.retrieve(parts);
}

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

    // an interface's own associated types, outermost of all: they belong to the whole body, and a
    // requirement's type-parameter frame - which is empty for an ordinary requirement - sits inside them
    if (associated_owner != nullptr) {
        if (TypeParamDecl *assoc = associated_owner->find_associated_type(name)) {
            return assoc;
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