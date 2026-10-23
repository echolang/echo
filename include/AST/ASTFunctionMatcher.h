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
    //
    // `argument_types` is the bound list in *parameter* order: names and defaults have already
    // been applied by AST::bind_arguments. it is required, not optional - a test without a tree
    // copies the written types onto every candidate
    struct FunctionCandidate
    {
        FunctionDeclNode *decl = nullptr;
        std::vector<ValueType> parameter_types;
        bool is_generic = false;
        std::vector<ValueType> argument_types;
        std::vector<ExprNode *> arguments;
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

            // pending: the arguments that would decide have no type yet, including a lone
            // candidate whose argument still names a type parameter. not an error: the call
            // is left unresolved for a later pass
            t_undecidable,
        };

        FunctionDeclNode *decl = nullptr;
        Outcome outcome = Outcome::t_no_candidates;

        // the candidates that tied, for the ambiguity diagnostic. also carries every viable
        // candidate when the outcome is undecidable
        std::vector<FunctionDeclNode *> tied;
    };

    // picks the candidate a call resolves to
    //
    // each candidate carries its own bound argument list in parameter order. CallResolver fills
    // those from AST::bind_arguments; tests copy the written types onto every candidate
    //
    // the rules, in order:
    //   1. arity must match exactly on the *bound* list - defaults and names are applied by
    //      AST::bind_arguments before this runs, so a hole that had a default is already filled. C
    //      variadic tails stay a last-parameter type, not a matcher exception
    //   2. if exactly one candidate survives arity, it wins *without* consulting types - unless an
    //      argument still names a type parameter. that call sits in a generic body and must stay
    //      pending so the clone can re-resolve against the consumer's overload set. a determined
    //      mismatch still binds, which is what keeps a program with no overloads reporting
    //      "argument 1 of 'f' expects int32". a written `null` is unknown, not a parameter
    //   3. a candidate is dropped if any argument does not fit it at all
    //   4. one candidate beats another only if it is no worse on every argument and better on at
    //      least one. wrapping into a tagged `T?` is a second key on the same payload rank, so
    //      `g(string)` beats `g(string?)` for `'a'` without flattening wrap-of-exact with
    //      wrap-of-promotion. a summed score would let a candidate win by being much better on one
    //      argument and slightly worse on another, which is how overload resolution becomes
    //      unpredictable
    //   5. only then does a non-generic candidate beat a generic one - how well the arguments fit
    //      is a statement about the call, "concrete or template" only about the declaration, so it
    //      gets a say once the call itself cannot tell them apart
    //   6. undetermined arguments are excluded from the comparison entirely. if what remains
    //      cannot separate the survivors, the answer is t_undecidable, not t_ambiguous
    FunctionMatch match_function(const std::vector<FunctionCandidate> &candidates);

    // renders "foo(int32, float64)" for a diagnostic listing what was tried
    std::string describe_candidates(const std::vector<FunctionDeclNode *> &candidates);

    // renders "a 'P'" / "a 'P' and a 'null'" - the *arguments* rather than the candidates, for a
    // diagnostic that must not name a declaration. beside describe_candidates because it is the other
    // half of the same choice: an operator's overload set is the whole program's, so a refusal about
    // one has to be worded from what the author wrote
    std::string describe_operands(const std::vector<ValueType> &operand_types);
};

#endif
