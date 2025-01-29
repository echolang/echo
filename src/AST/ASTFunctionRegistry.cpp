#include "AST/ASTFunctionRegistry.h"

#include "AST/ASTCollector.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTNamespace.h"
#include "AST/FunctionDeclNode.h"

#include <fmt/core.h>

#include <cassert>

// compared one parameter at a time so a mismatch stops at the first, rather than building a vector
// per candidate - this runs for every declaration in the bundle
bool AST::signatures_match(const AST::FunctionDeclNode *candidate, const std::vector<AST::ValueType> &parameter_types)
{
    if (candidate->args.size() != parameter_types.size()) {
        return false;
    }

    for (size_t i = 0; i < parameter_types.size(); i++) {
        if (!(candidate->parameter_type(i) == parameter_types[i])) {
            return false;
        }
    }

    return true;
}

namespace
{
    // "this symbol is already declared with these parameter types". one wording, so the free and the
    // member path cannot drift apart the first time it is improved
    void report_duplicate_signature(
        AST::Collector &collector, const AST::CodeRef &at, const AST::FunctionDeclNode *previous)
    {
        collector.collect_issue<AST::Issue::DuplicateFunctionSignature>(
            at,
            fmt::format(
                "'{}' is already declared with these parameter types. Overloads must differ in their parameters.",
                previous->signature_description()));
    }
}

bool AST::FunctionRegistry::claim_declaration_site(AST::FunctionDeclNode *decl)
{
    if (decl == nullptr || decl->is_anonymous()) {
        return false;
    }

    const DeclarationSite site = make_declaration_site(decl->declaration_site_token());

    // the same declaration coming back around in the module's second parse pass. everything the
    // caller would do next has already happened for it, including any duplicate report - doing it
    // again would both double the diagnostic and add the declaration to its own overload set twice
    if (const auto claimed = _by_decl_site.find(site); claimed != _by_decl_site.end()) {
        // ...and "the same declaration" means the very same node: every parser that can reach a
        // declaration twice looks the site up first and carries on with what it finds, so a second
        // node at one site would mean the identity is wrong. the failure would otherwise be silent,
        // this path reading it as "the second pass came back around" and dropping the loser from
        // its overload set
        assert(claimed->second == decl && "two declarations claim one declaration site");

        return false;
    }

    _functions.push_back(decl);
    _by_decl_site[site] = decl;

    return true;
}

void AST::FunctionRegistry::register_function(
    AST::Collector &collector, const AST::CodeRef &at, AST::FunctionDeclNode *decl)
{
    if (!claim_declaration_site(decl)) {
        return;
    }

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
        report_duplicate_signature(collector, at, previous);

        // deliberately not added to the overload set: leaving it out keeps resolution
        // deterministic (the first declaration wins) instead of making every later call ambiguous
        return;
    }

    _by_name[ns][name].push_back(decl);
}

void AST::FunctionRegistry::register_member_function(
    AST::Collector &collector, const AST::CodeRef &at, AST::FunctionDeclNode *decl, AST::ComplexType &owner)
{
    // the declaration site is a real token here (a method is written where its name is written,
    // unlike a constructor), so the two passes reconcile without a virtual token
    if (!claim_declaration_site(decl)) {
        return;
    }

    // duplicate detection against the owner rather than against a namespace, because that is where
    // a method is reachable from. same diagnostic either way: a source error, not the mangling
    // defect TypeLowering would otherwise throw
    if (auto *previous = find_member_by_signature(owner, decl, decl)) {
        report_duplicate_signature(collector, at, previous);

        // not added, so the first declaration wins and resolution stays deterministic
        return;
    }

    // deliberately not entered into _by_name: a method is reached through its receiver, so a bare
    // `push(...)` must not resolve to it. that is the whole difference from register_function
    owner.add_method(decl);
}

void AST::FunctionRegistry::register_destructor(
    AST::Collector &collector, const AST::CodeRef &at, AST::FunctionDeclNode *decl, AST::ComplexType &owner)
{
    // the `destructor` keyword is a real token at a fixed index, so the two passes reconcile on it
    // exactly as a method reconciles on its name token. an unclaimed site means the body pass coming
    // back around to the node the declaration pass registered - there is nothing left to do
    if (!claim_declaration_site(decl)) {
        return;
    }

    // a struct has at most one, and there are no parameters to overload on - so the slot itself is
    // the duplicate check, and it is the caller's, which owns the wording. entered in neither
    // _by_name nor the method table: a destructor is not a name anybody can write, and it must not
    // turn up as a candidate in an overload diagnostic
    owner.set_destructor(decl);
}

AST::FunctionDeclNode *AST::FunctionRegistry::find_member_by_signature(
    const AST::ComplexType &owner,
    const AST::FunctionDeclNode *decl,
    const AST::FunctionDeclNode *ignore) const
{
    // materialized once for the whole search rather than per candidate, which is what keeps
    // signatures_match's parameter-at-a-time comparison worth having
    const std::vector<ValueType> parameter_types = decl->parameter_types();

    for (auto *candidate : find_member_functions(&owner, decl->func_name())) {
        if (candidate == ignore) {
            continue;
        }

        if (signatures_match(candidate, parameter_types)) {
            return candidate;
        }
    }

    return nullptr;
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

AST::FunctionDeclNode *AST::FunctionRegistry::find_by_declaration_site(const TokenReference &declaration_token) const
{
    const auto found = _by_decl_site.find(make_declaration_site(declaration_token));
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
        if (candidate == ignore) {
            continue;
        }

        if (signatures_match(candidate, parameter_types)) {
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
