#include "AST/ASTContext.h"

#include "AST/ASTTypeParam.h"
#include "AST/FunctionDeclNode.h"

#include <fmt/core.h>

// everything outside `[A-Za-z0-9_]` becomes an underscore. A file stem may hold a dash, a dot or worse,
// and these end up inside an emitted symbol name - LLVM would quote them, but a symbol a developer has to
// read in a linker error or a profile is worth keeping plain
static std::string sanitize_symbol_fragment(const std::string &raw)
{
    std::string out;
    out.reserve(raw.size());

    for (const char c : raw) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out.push_back(safe ? c : '_');
    }

    return out;
}

std::string AST::Context::site_discriminator(const TokenReference &at) const
{
    // a module always has files by the time anything is minted, but a test harness can build a Context
    // over one that does not - and an empty tag is still unique per line and column within one file,
    // which is all a single-file module needs
    const std::string file_tag =
        file.file != nullptr ? sanitize_symbol_fragment(file.file->get_path().stem().string()) : "";

    return fmt::format("{}L{}C{}", file_tag, at.line(), at.char_offset());
}

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
        *context.current_namespace, make_declaration_site(block_token.value()), display_name,
        context.site_discriminator(block_token.value()));
}

AST::MemberTypeScope::MemberTypeScope(
    AST::Context &context,
    AST::NamespaceManager &namespaces,
    const AST::ComplexType &owner) :
    context(context), previous(context.current_namespace)
{
    // through the one owner of that question, which a struct's compile-time constants are published into
    // as well - so `A::Inner(1)` and `buffer::MAX` cannot end up in two different namespaces.
    // leaving the namespace alone is the safe answer for an anonymous owner, which has no path at all
    if (AST::Namespace *surface = AST::member_surface_namespace(namespaces, owner)) {
        context.current_namespace = surface;
    }
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