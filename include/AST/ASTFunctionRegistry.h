#ifndef ASTFUNCTIONREGISTRY_H
#define ASTFUNCTIONREGISTRY_H

#pragma once

#include "AST/ASTCodeRef.h"
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

    // the single store of function declarations, bundle-wide, keyed by namespace and name to a
    // *set* of overloads rather than to one declaration.
    //
    // it replaces two maps that were keyed on the bare name and silently overwrote each other:
    // ScopeNode::_declared_functions and the function half of Namespace::_symbols. Namespace
    // symbols are types only now, which is also what lets a struct `Foo` and its constructor
    // `Foo` stop sharing one slot.
    class FunctionRegistry
    {
    public:
        // the declaration a module's two parse passes agree on. a module is parsed twice - once
        // for symbols, once in full - over the same tokens, so the name token's position is an
        // exact identity for "this declaration" that is available *before* the parameter list is
        // parsed. the name alone is not: with overloads it names a set, and the reuse decision
        // has to happen at the name token, long before the signature is known
        struct DeclarationSite {
            const TokenCollection *tokens;
            size_t index;

            bool operator==(const DeclarationSite &other) const {
                return tokens == other.tokens && index == other.index;
            }
        };

        struct DeclarationSiteHash {
            size_t operator()(const DeclarationSite &site) const {
                return std::hash<const TokenCollection *>{}(site.tokens) ^ (std::hash<size_t>{}(site.index) << 1);
            }
        };

        FunctionRegistry() {};

        ~FunctionRegistry() {};

        // registers a declaration under its own namespace and name. re-registering the same
        // declaration site is a no-op rather than a duplicate, which is what makes the symbol
        // pass and the full pass idempotent. a *different* declaration with parameter types the
        // set already holds is reported on the collector as a duplicate.
        void register_function(Collector &collector, const CodeRef &at, FunctionDeclNode *decl);

        // the overload set for a name, searched from `ns` outward. the first namespace holding
        // any candidate for the name answers - an outer namespace does not extend an inner one's
        // set, it is hidden by it. empty when the name is unknown everywhere.
        std::vector<FunctionDeclNode *> overloads(const std::string &name, const Namespace &ns) const;

        // the declaration already registered for this name token, or null on the first pass over
        // it. this is the symbol-pass / full-pass reconciliation.
        FunctionDeclNode *find_by_declaration_site(const TokenReference &name_token) const;

        // does an overload with exactly these parameter types already exist for (ns, name)?
        // `ignore` is skipped, so a declaration can ask without matching itself.
        FunctionDeclNode *find_by_signature(
            const std::string &name,
            const Namespace &ns,
            const std::vector<ValueType> &parameter_types,
            const FunctionDeclNode *ignore = nullptr) const;

        // every registered declaration, in declaration order
        inline const std::vector<FunctionDeclNode *> &get_all() const {
            return _functions;
        }

        std::string debug_dump() const;

    private:

        // declaration order, so a diagnostic or a dump listing declarations is reproducible across
        // runs - the namespace and name maps are unordered and would not be
        std::vector<FunctionDeclNode *> _functions;

        std::unordered_map<const Namespace *, std::unordered_map<std::string, std::vector<FunctionDeclNode *>>> _by_name;

        std::unordered_map<DeclarationSite, FunctionDeclNode *, DeclarationSiteHash> _by_decl_site;
    };
};

#endif
