#include "AST/ASTFunctionRegistry.h"

#include "AST/ASTCollector.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTNamespace.h"
#include "AST/FunctionDeclNode.h"

#include <fmt/core.h>

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

bool AST::signatures_match(const AST::FunctionDeclNode *a, const AST::FunctionDeclNode *b)
{
    if (a == nullptr || b == nullptr || a->args.size() != b->args.size()) {
        return false;
    }

    for (size_t i = 0; i < a->args.size(); i++) {
        if (!(a->parameter_type(i) == b->parameter_type(i))) {
            return false;
        }

        const AST::VarDeclNode *pa = a->args[i];
        const AST::VarDeclNode *pb = b->args[i];
        const std::string la = pa != nullptr ? pa->label() : "";
        const std::string lb = pb != nullptr ? pb->label() : "";

        if (la != lb) {
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
                "'{}' is already declared with these parameter types. Overloads must differ in their parameters or labels.",
                previous->signature_description()
            ));
    }
}

bool AST::FunctionRegistry::claim_declaration_site(
    AST::Collector &collector,
    const AST::CodeRef &at,
    AST::FunctionDeclNode *decl
)
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
        //
        // reported rather than asserted, because it is reachable from *malformed* input: the passes
        // walk the same tokens, so anything that makes one of them read a different block structure
        // than another lands here. an assert gives no location and, under NDEBUG, is exactly the
        // silent path above. this is a defect in the compiler either way, and the location is the
        // only thing anybody can act on
        if (claimed->second != decl) {
            collector.collect_issue<AST::Issue::GenericError>(
                at,
                fmt::format(
                    "'{}' was parsed as two separate declarations at one declaration site - the parse "
                    "passes disagree about the structure of this file. This is a defect in the compiler.",
                    decl->func_name()));
        }

        return false;
    }

    _functions.push_back(decl);
    _by_decl_site[site] = decl;

    return true;
}

void AST::FunctionRegistry::register_function(
    AST::Collector &collector,
    const AST::CodeRef &at,
    AST::FunctionDeclNode *decl
)
{
    if (!claim_declaration_site(collector, at, decl)) {
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
    if (auto *previous = first_matching_signature(declared_overloads(name, *ns), decl, decl)) {
        report_duplicate_signature(collector, at, previous);

        // deliberately not added to the overload set: leaving it out keeps resolution
        // deterministic (the first declaration wins) instead of making every later call ambiguous
        return;
    }

    _by_name[ns][name].push_back(decl);
}

void AST::FunctionRegistry::register_member_function(
    AST::Collector &collector,
    const AST::CodeRef &at,
    AST::FunctionDeclNode *decl,
    AST::ComplexType &owner
)
{
    // the declaration site is a real token here (a method is written where its name is written,
    // unlike a constructor), so the two passes reconcile without a virtual token
    if (!claim_declaration_site(collector, at, decl)) {
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

void AST::FunctionRegistry::register_static_function(
    AST::Collector &collector,
    const AST::CodeRef &at,
    AST::FunctionDeclNode *decl,
    AST::ComplexType &owner
)
{
    // a static is written where its name is written, exactly as a method is, so the two passes
    // reconcile on a real token with no virtual one minted
    if (!claim_declaration_site(collector, at, decl)) {
        return;
    }

    // **against the static list alone**, which is the whole reason this is not register_member_function
    // with a flag: a `static function get()` and a `function get()` on one type are two declarations a
    // reader can tell apart at both call sites, so they do not collide. checking across the two would
    // refuse a pair the language admits
    if (auto *previous = find_static_by_signature(owner, decl, decl)) {
        report_duplicate_signature(collector, at, previous);

        // not added, so the first declaration wins and resolution stays deterministic
        return;
    }

    // neither in _by_name nor in the method table - see the comment on ComplexType::add_static_method
    // for why the second of those matters as much as the first
    owner.add_static_method(decl);
}

void AST::FunctionRegistry::register_destructor(
    AST::Collector &collector,
    const AST::CodeRef &at,
    AST::FunctionDeclNode *decl,
    AST::ComplexType &owner
)
{
    // the `destructor` keyword is a real token at a fixed index, so the two passes reconcile on it
    // exactly as a method reconciles on its name token. an unclaimed site means the body pass coming
    // back around to the node the declaration pass registered - there is nothing left to do
    if (!claim_declaration_site(collector, at, decl)) {
        return;
    }

    // a struct has at most one, and there are no parameters to overload on - so the slot itself is
    // the duplicate check, and it is the caller's, which owns the wording. entered in neither
    // _by_name nor the method table: a destructor is not a name anybody can write, and it must not
    // turn up as a candidate in an overload diagnostic
    owner.set_destructor(decl);
}

void AST::FunctionRegistry::register_type_init(
    AST::Collector &collector,
    const AST::CodeRef &at,
    AST::FunctionDeclNode *decl,
    AST::ComplexType &owner
)
{
    if (!claim_declaration_site(collector, at, decl)) {
        return;
    }

    owner.set_type_init(decl);
}

AST::FunctionDeclNode *AST::FunctionRegistry::find_member_by_signature(
    const AST::ComplexType &owner,
    const AST::FunctionDeclNode *decl,
    const AST::FunctionDeclNode *ignore
) const
{
    return first_matching_signature(find_member_functions(&owner, decl->func_name()), decl, ignore);
}

AST::FunctionDeclNode *AST::FunctionRegistry::find_static_by_signature(
    const AST::ComplexType &owner,
    const AST::FunctionDeclNode *decl,
    const AST::FunctionDeclNode *ignore
) const
{
    return first_matching_signature(find_static_functions(&owner, decl->func_name()), decl, ignore);
}

AST::FunctionDeclNode *AST::FunctionRegistry::first_matching_signature(
    const std::vector<AST::FunctionDeclNode *> &candidates,
    const AST::FunctionDeclNode *decl,
    const AST::FunctionDeclNode *ignore
) const
{
    for (auto *candidate : candidates) {
        if (candidate == ignore) {
            continue;
        }

        if (signatures_match(candidate, decl)) {
            return candidate;
        }
    }

    return nullptr;
}

const std::vector<AST::FunctionDeclNode *> &AST::FunctionRegistry::overloads(
    const std::string &name,
    const AST::Namespace &ns
) const
{
    // the answer for a name nothing declares. one object rather than a fresh empty vector per ask, and
    // what lets this hand back a reference at all
    static const std::vector<FunctionDeclNode *> none;


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

    return none;
}

const std::vector<AST::FunctionDeclNode *> &AST::FunctionRegistry::declared_overloads(
    const std::string &name,
    const AST::Namespace &ns
) const
{
    static const std::vector<FunctionDeclNode *> none;

    const auto in_namespace = _by_name.find(&ns);
    if (in_namespace == _by_name.end()) {
        return none;
    }

    const auto candidates = in_namespace->second.find(name);
    if (candidates == in_namespace->second.end()) {
        return none;
    }

    return candidates->second;
}

bool AST::FunctionRegistry::declares(const std::string &name, const AST::Namespace &ns) const
{
    return !declared_overloads(name, ns).empty();
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
    const AST::FunctionDeclNode *ignore
) const
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
