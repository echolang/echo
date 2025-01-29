#ifndef ASTFUNCTIONREGISTRY_H
#define ASTFUNCTIONREGISTRY_H

#pragma once

#include "AST/ASTCodeRef.h"
#include "AST/ASTDeclarationSite.h"
#include "AST/ASTValueType.h"
#include "Token.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace AST
{
    class Collector;
    class FunctionDeclNode;
    class Namespace;

    // "are these the same symbol", parameter by parameter - the identity question, as distinct from
    // AST::argument_fit's "can this argument reach this parameter". ValueType equality is exact by
    // design (it is the interning identity the type registry and the monomorphizer's instance cache
    // use), which is precisely what "the same signature" has to mean
    //
    // exposed because the struct parser asks it too, of a struct's own constructors, when deciding
    // whether the field-wise constructor would duplicate one the user wrote
    bool signatures_match(const FunctionDeclNode *candidate, const std::vector<ValueType> &parameter_types);

    // the single store of function declarations, bundle-wide, keyed by namespace and name to a
    // *set* of overloads rather than to one declaration
    //
    // it replaces two maps that were keyed on the bare name and silently overwrote each other:
    // ScopeNode::_declared_functions and the function half of Namespace::_symbols. Namespace
    // symbols are types only now, which is also what lets a struct `Foo` and its constructor
    // `Foo` stop sharing one slot
    class FunctionRegistry
    {
    public:
        FunctionRegistry() {};

        ~FunctionRegistry() {};

        // registers a declaration under its own namespace and name. re-registering the same
        // declaration site is a no-op rather than a duplicate, which is what makes the symbol
        // pass and the full pass idempotent. a *different* declaration with parameter types the
        // set already holds is reported on the collector as a duplicate
        void register_function(Collector &collector, const CodeRef &at, FunctionDeclNode *decl);

        // registers a *member* function on its owning type. reconcilable across the two parse
        // passes by declaration site and duplicate-checked against the owner's existing methods,
        // exactly as register_function is - but deliberately never entered into the (namespace,
        // name) overload sets, because a method is reached through its receiver. a bare `push(...)`
        // therefore does not resolve to `Stack::push`, and AST::find_member_functions is the only
        // way to it
        void register_member_function(Collector &collector, const CodeRef &at, FunctionDeclNode *decl, ComplexType &owner);

        // registers the owner's destructor. reconciles across the parse passes by declaration site
        // like the two above, but enters the declaration in *neither* lookup structure: the method
        // table would make it an overload candidate, and it is not a name anyone can write - the
        // drop sites AST::OwnershipPass inserts reach it through AST::find_destructor.
        //
        // there is nothing to duplicate-check here: a struct has one destructor slot and no parameters
        // to overload on, so "already has a destructor" is the caller's report to make, where the
        // struct's name is at hand - and the caller asks it of the slot, before registering
        void register_destructor(Collector &collector, const CodeRef &at, FunctionDeclNode *decl, ComplexType &owner);

        // the overload set for a name, searched from `ns` outward. the first namespace holding
        // any candidate for the name answers - an outer namespace does not extend an inner one's
        // set, it is hidden by it. empty when the name is unknown everywhere
        std::vector<FunctionDeclNode *> overloads(const std::string &name, const Namespace &ns) const;

        // the declaration already registered as written at this token, or null on the first pass
        // over it. this is the declaration-pass / body-pass reconciliation
        FunctionDeclNode *find_by_declaration_site(const TokenReference &declaration_token) const;

        // does an overload with exactly these parameter types already exist for (ns, name)?
        // `ignore` is skipped, so a declaration can ask without matching itself
        FunctionDeclNode *find_by_signature(
            const std::string &name,
            const Namespace &ns,
            const std::vector<ValueType> &parameter_types,
            const FunctionDeclNode *ignore = nullptr) const;

        // the member counterpart: does `owner` already declare a method with `decl`'s name and
        // exactly its parameter types (the receiver included)? `ignore` is skipped so a declaration
        // can ask without matching itself
        //
        // takes the declaration rather than a name and a type vector, so neither side has to be
        // materialized to ask - the comparison stops at the first differing parameter
        FunctionDeclNode *find_member_by_signature(
            const ComplexType &owner,
            const FunctionDeclNode *decl,
            const FunctionDeclNode *ignore = nullptr) const;

        // every registered declaration, in declaration order
        inline const std::vector<FunctionDeclNode *> &get_all() const {
            return _functions;
        }

        std::string debug_dump() const;

    private:

        // takes ownership of `decl`'s declaration site: appends it to `_functions`, keys it in
        // `_by_decl_site` and answers whether the caller should carry on registering it. false for a
        // declaration that cannot be looked up (null or anonymous) and for the second parse pass
        // reaching a site already claimed - the shared prologue of both register_ entry points, so
        // the two-pass idempotency rule is stated exactly once
        bool claim_declaration_site(FunctionDeclNode *decl);

        // declaration order, so a diagnostic or a dump listing declarations is reproducible across
        // runs - the namespace and name maps are unordered and would not be
        std::vector<FunctionDeclNode *> _functions;

        std::unordered_map<const Namespace *, std::unordered_map<std::string, std::vector<FunctionDeclNode *>>> _by_name;

        // keyed on the declaration a module's parse passes agree on, the same key AST::NamespaceManager
        // keys a lexical namespace by. which token identifies *this* kind of declaration is
        // FunctionDeclNode::declaration_site_token()'s answer: the name token for anything the user
        // named, its own `constructor` keyword for a constructor
        std::unordered_map<DeclarationSite, FunctionDeclNode *, DeclarationSiteHash> _by_decl_site;
    };
};

#endif
