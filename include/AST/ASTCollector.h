#ifndef ASTCOLLECTOR_H
#define ASTCOLLECTOR_H

#pragma once

#include "AST/ASTValueTypeCollection.h"
#include "AST/ASTIssue.h"
#include "AST/ASTContext.h"
#include "AST/ASTFunctionRegistry.h"
#include "AST/ASTOps.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTValueType.h"
#include "AST/ASTTypeParam.h"

#include <set>
#include <tuple>
#include <typeindex>

namespace AST
{
    class Collector
    {
    public:
        std::vector<std::unique_ptr<AST::IssueRecord>> issues;
        ValueTypeCollection value_types = ValueTypeCollection();
        OperatorRegistry operators = OperatorRegistry();
        NamespaceManager namespaces = NamespaceManager();
        TypeRegistry type_registry = TypeRegistry();
        TypeParamRegistry type_params = TypeParamRegistry();

        // every function declaration in the bundle, as overload sets. `namespaces` holds the
        // types; this holds the functions, which is what lets a struct `Foo` and its constructor
        // `Foo` coexist under one name
        FunctionRegistry functions = FunctionRegistry();

        // names the next closure literal. a closure has no name of its own and is in no overload set, but
        // it still needs a *symbol*, and two closures written in one block would otherwise mangle alike.
        // bundle-wide and deterministic: files are walked in a fixed order, and a literal is parsed once
        size_t next_closure_id = 0;
        
        // create a registry for the native scalar cast types
        
        Collector();
        ~Collector();

        // a module is parsed more than once - a declaration pass for the signatures, then a full
        // pass for the bodies - and the passes are deliberately the same code, so a malformed
        // declaration is reported once per pass. de-duplicated here, at the one place every issue
        // travels through, rather than by teaching each reporting site which pass it is in
        template <typename T, typename... Args>
        void collect_issue(const CodeRef &code_ref, Args... args) {
            auto issue = std::make_unique<T>(code_ref, args...);

            // two issues are the same issue when they are the same kind, about the same tokens, and
            // say the same thing. the token collection belongs in the key because indices are per
            // module - the same shape AST::DeclarationSite keys a declaration on
            const bool is_first = _reported.emplace(
                std::type_index(typeid(T)),
                &code_ref.token_slice.tokens,
                code_ref.token_slice.start_index,
                code_ref.token_slice.end_index,
                issue->message()).second;

            if (!is_first) {
                return;
            }

            issues.push_back(std::move(issue));
        }

        void print_issues() const;

        bool has_critical_issues() const;

    private:

        std::set<std::tuple<std::type_index, const TokenCollection *, size_t, size_t, std::string>> _reported;
    };
};

#endif