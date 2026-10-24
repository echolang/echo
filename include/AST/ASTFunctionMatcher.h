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

    // picks the candidate a call resolves to. CallResolver fills each candidate's bound list.
    //   1. arity matches the bound list exactly
    //   2. one survivor of arity wins without types, unless an argument still names a type parameter
    //   3. drop a candidate any argument does not fit
    //   4. Pareto: no worse on every argument, better on at least one (wrap into `T?` is a second key)
    //   5. then concrete beats template
    //   6. undetermined arguments are excluded; leftover ties are t_undecidable, not t_ambiguous
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
