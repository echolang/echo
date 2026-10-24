#ifndef ASTINTERPOLATIONLOWERING_H
#define ASTINTERPOLATIONLOWERING_H

#pragma once

#include "AST/ASTFixpointLowering.h"

#include "Token.h"

#include <string>
#include <vector>

namespace AST
{
    class Bundle;
    class ExprNode;
    class Namespace;
    class StringInterpolationExprNode;

    // `"a{$x}b"` becomes one local and a sequence of `append`s over `str::from`. a hole is a
    // unary call, so match_function's arity rule is untouched. a single piece stays the
    // expression it is. inside the fixpoint, before ownership. no pending state, so no
    // `finalize()`
    class InterpolationLowering : private FixpointLowering
    {
    public:
        InterpolationLowering(Bundle &bundle);

        using FixpointLowering::run_round;

    private:
        // the one edge this pass treats differently. the base's descent runs **first**, so a hole
        // holding an interpolation of its own is already lowered by the time this one appends - which
        // is what makes `"{$a}{$b}"` inside `"{$c}"` terminate
        ExprNode *rewrite_value_edge(ExprNode *expr) override;

        // the replacement for `node`, or the literal it is refused down to
        ExprNode *lower(StringInterpolationExprNode &node);

        // reports, discards the holes, and answers the chunks as one plain literal - so a program
        // without a standard library gets one diagnostic rather than a cascade about `str`
        ExprNode *refuse(StringInterpolationExprNode &node, std::string why);

        // a plain string literal standing for part of this one: a chunk, a hole's format spec, or the
        // whole thing when it was refused. **`at` is what the three differ by** - a spec belongs at its
        // own hole so a diagnostic about it underlines the right bracket - and it defaults to the
        // literal, which is where a chunk and a refusal both point. Every one of them owes the same
        // `decoded_value` and `core_string_type`, so there is one place that sets both
        ExprNode &literal_for(
            const StringInterpolationExprNode &node,
            const std::string &bytes,
            const TokenReference *at = nullptr
        );

        // a free call in namespace `str`, left unresolved for the fixpoint's own settle_calls to
        // finish. named `from` rather than a member so a user's own `str::from(MyType)` overload
        // wins, which is the `hash::of` bargain
        ExprNode &str_call(
            const std::string &name,
            const TokenReference &at,
            std::vector<ExprNode *> operands
        );
    };
};

#endif
