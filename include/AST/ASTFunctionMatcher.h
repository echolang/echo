#ifndef ASTFUNCTIONMATCHER_H
#define ASTFUNCTIONMATCHER_H

#pragma once

#include "AST/ASTArgumentFit.h"
#include "AST/ASTValueType.h"

#include <string>
#include <vector>

namespace AST
{
    class ExprNode;
    class FunctionDeclNode;

    // one candidate of an overload set, reduced to what ranking needs. a view rather than the
    // declaration itself, so the matcher can be exercised without a parse tree - and so the
    // generic case can substitute a template's parameter types before they are compared
    struct FunctionCandidate
    {
        FunctionDeclNode *decl = nullptr;
        std::vector<ValueType> parameter_types;
        bool is_generic = false;
    };

    struct FunctionMatch
    {
        enum class Outcome
        {
            // exactly one candidate answers the call
            t_resolved,

            // the name has no declarations at all
            t_no_candidates,

            // candidates exist, none of them accepts these arguments
            t_no_viable,

            // several candidates accept the arguments and none is better than the rest
            t_ambiguous,

            // several candidates remain and the arguments that would tell them apart have no
            // type yet. not an error: the call is left unresolved for a later pass, because
            // reporting it here would reject a program that is perfectly well typed
            t_undecidable,
        };

        FunctionDeclNode *decl = nullptr;
        Outcome outcome = Outcome::t_no_candidates;

        // the candidates that tied, for the ambiguity diagnostic. also carries every viable
        // candidate when the outcome is undecidable
        std::vector<FunctionDeclNode *> tied;
    };

    // picks the candidate a call resolves to.
    //
    // `argument_types` and `arguments` are parallel; `arguments` may hold nulls (or be shorter)
    // when only the types are known, which is what keeps the matcher testable without a tree.
    //
    // the rules, in order:
    //   1. arity must match exactly - Echo has no default parameters and no varargs
    //   2. if exactly one candidate survives arity, it wins *without* consulting types. that is
    //      what makes a program with no overloads behave precisely as it did before overload
    //      resolution existed, right down to which pass reports a bad argument: the type checker
    //      still says "argument 1 of 'f' expects int32", rather than this returning t_no_viable
    //   3. a candidate is dropped if any argument does not fit it at all
    //   4. one candidate beats another only if it is no worse on every argument and better on at
    //      least one. a summed score would let a candidate win by being much better on one
    //      argument and slightly worse on another, which is how overload resolution becomes
    //      unpredictable
    //   5. only then does a non-generic candidate beat a generic one - how well the arguments fit
    //      is a statement about the call, "concrete or template" only about the declaration, so it
    //      gets a say once the call itself cannot tell them apart
    //   6. undetermined arguments are excluded from the comparison entirely. if what remains
    //      cannot separate the survivors, the answer is t_undecidable, not t_ambiguous
    FunctionMatch match_function(
        const std::vector<FunctionCandidate> &candidates,
        const std::vector<ValueType> &argument_types,
        const std::vector<ExprNode *> &arguments);

    // renders "foo(int32, float64)" for a diagnostic listing what was tried
    std::string describe_candidates(const std::vector<FunctionDeclNode *> &candidates);
};

#endif
