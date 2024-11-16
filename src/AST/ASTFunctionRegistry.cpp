#include "AST/ASTFunctionRegistry.h"

#include "AST/ASTCollector.h"
#include "AST/ASTNamespace.h"
#include "AST/FunctionDeclNode.h"

#include <fmt/core.h>

void AST::FunctionRegistry::register_function(
    AST::Collector &collector, const AST::CodeRef &at, AST::FunctionDeclNode *decl)
{
    if (decl == nullptr || decl->is_anonymous()) {
        return;
    }

    const auto &name_token = decl->name_token.value();
    const DeclarationSite site { &name_token.get_collection_ref(), name_token.get_handle() };

    // the same declaration coming back around in the module's second parse pass. everything below
    // has already happened for it, including any duplicate report - doing it again would both
    // double the diagnostic and add the declaration to its own overload set twice
    if (_by_decl_site.count(site) > 0) {
        return;
    }

    _functions.push_back(decl);
    _by_decl_site[site] = decl;

    const Namespace *ns = decl->ast_namespace;

    // a declaration with no namespace cannot be looked up by name, but it still gets a declaration
    // site so the two passes reconcile
    if (ns == nullptr) {
        return;
    }

    const std::string name = decl->func_name();

    // two declarations that differ only in a way the symbol table cannot see are not overloads,
    // they are the same symbol twice. catching it here rather than at the call site is what turns
    // TypeLowering's "this is a name mangling defect, not a source error" throw into a located
    // source error the user can act on
    if (auto *previous = find_by_signature(name, *ns, decl->parameter_types(), decl)) {
        collector.collect_issue<AST::Issue::DuplicateFunctionSignature>(
            at,
            fmt::format(
                "'{}' is already declared with these parameter types. Overloads must differ in their parameters.",
                previous->signature_description()));

        // deliberately not added to the overload set: leaving it out keeps resolution
        // deterministic (the first declaration wins) instead of making every later call ambiguous
        return;
    }

    _by_name[ns][name].push_back(decl);
}

std::vector<AST::FunctionDeclNode *> AST::FunctionRegistry::overloads(
    const std::string &name, const AST::Namespace &ns) const
{
    // innermost first. the first namespace that has *any* candidate for the name answers it
    // entirely - an outer namespace does not extend an inner one's overload set, it is hidden by
    // it, exactly as a local variable hides an outer one of the same name
    for (const Namespace *current = &ns; current != nullptr; current = current->parent()) {
        const auto in_namespace = _by_name.find(current);
        if (in_namespace == _by_name.end()) {
            continue;
        }

        const auto candidates = in_namespace->second.find(name);
        if (candidates == in_namespace->second.end() || candidates->second.empty()) {
            continue;
        }

        return candidates->second;
    }

    return {};
}

AST::FunctionDeclNode *AST::FunctionRegistry::find_by_declaration_site(const TokenReference &name_token) const
{
    const DeclarationSite site { &name_token.get_collection_ref(), name_token.get_handle() };

    const auto found = _by_decl_site.find(site);
    return found != _by_decl_site.end() ? found->second : nullptr;
}

AST::FunctionDeclNode *AST::FunctionRegistry::find_by_signature(
    const std::string &name,
    const AST::Namespace &ns,
    const std::vector<AST::ValueType> &parameter_types,
    const AST::FunctionDeclNode *ignore) const
{
    // this namespace only - hiding is a lookup rule, and a declaration in an inner namespace does
    // not collide with one of the same shape further out
    const auto in_namespace = _by_name.find(&ns);
    if (in_namespace == _by_name.end()) {
        return nullptr;
    }

    const auto candidates = in_namespace->second.find(name);
    if (candidates == in_namespace->second.end()) {
        return nullptr;
    }

    for (auto *candidate : candidates->second) {
        if (candidate == ignore || candidate->args.size() != parameter_types.size()) {
            continue;
        }

        // ValueType equality is exact by design (it is the interning identity the type registry
        // and the monomorphizer's instance cache use), which is precisely what "the same
        // signature" needs to mean here. compared parameter by parameter, so a mismatch stops at
        // the first one instead of building a vector per candidate - this runs for every
        // declaration in the bundle
        bool same_signature = true;
        for (size_t i = 0; i < parameter_types.size() && same_signature; i++) {
            same_signature = candidate->parameter_type(i) == parameter_types[i];
        }

        if (same_signature) {
            return candidate;
        }
    }

    return nullptr;
}

std::string AST::FunctionRegistry::debug_dump() const
{
    std::string buffer;

    // declaration order, so this dump is stable across runs - the namespace and name maps are
    // unordered and would not be
    for (const auto *decl : _functions) {
        buffer += fmt::format("- {} -> {}\n", decl->signature_description(), decl->get_return_type().get_type_desciption());
    }

    return buffer;
}
