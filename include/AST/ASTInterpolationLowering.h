#ifndef ASTINTERPOLATIONLOWERING_H
#define ASTINTERPOLATIONLOWERING_H

#pragma once

#include "AST/ASTRecursiveVisitor.h"

#include "Token.h"

#include <string>
#include <vector>

namespace AST
{
    class Bundle;
    class Collector;
    struct CodeRef;
    class ExprNode;
    class File;
    class FunctionDeclNode;
    class Module;
    class Namespace;
    class StringInterpolationExprNode;

    // rewrites every interpolated string literal into the calls a hand written program would have
    // spelled:
    //
    //     "a{$x}b{$y:.2f}"
    //
    //     ==>  str::concat(str::concat(str::concat("a", str::from($x)), "b"), str::from($y, ".2f"))
    //
    // **that is the whole of what interpolation is**, and stating it here rather than in the parser is
    // what makes it one decision: the fold can become a builder, or `str::from` can be given a second
    // overload, without a single call site moving.
    //
    // and it is why nothing in this feature needs a variadic function. a hole is a *unary* call, so the
    // exact-arity rule AST::match_function has always had is untouched - the alternative reading, a
    // `fmt(format, ...)` taking the holes as arguments, is the one that would have needed one.
    //
    // **inside AST::Monomorphizer's fixpoint**, beside AST::ForeachLowering. it mints calls, and one of
    // them may name a user's own `str::from(MyType)` - so it has to be somewhere settle_calls still
    // runs after it. and it must precede the ownership pass, which walks a body exactly once, ever:
    // every call minted here returns an owning `string`, and a body walked before they exist is a body
    // whose drops were decided against a tree that had not arrived.
    //
    // **unlike the other three rewriters it has no pending state and so no finalize().** it never
    // needs a type: which overload `str::from` resolves to is AST::CallResolver's question, asked
    // whenever the argument settles. so this pass either lowers a literal or refuses it, in the first
    // round that reaches it, and there is no "not decided yet" to give an exit to
    class InterpolationLowering : private RecursiveVisitor
    {
    public:
        InterpolationLowering(Bundle &bundle);

        // answers whether anything changed, so the fixpoint can report progress
        bool run_round();

    private:
        CodeRef code_ref_for(const TokenReference &token);

        // the one edge this pass treats differently. the base's descent runs **first**, so a hole
        // holding an interpolation of its own is already lowered by the time this one folds - which
        // is what makes `"{$a}{$b}"` inside `"{$c}"` terminate
        ExprNode *rewrite_value_edge(ExprNode *expr) override;

        // a template's body is only meaningful once cloned into a concrete instance, and a call minted
        // into one would be resolved against type parameters nothing has bound. AST::ForeachLowering's
        // rule, and AST::PointerAdjuster's before it
        void visitFunctionDecl(FunctionDeclNode &node) override;

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
        // finish. free rather than a member call on `string` deliberately: a receiver has to be
        // addressed, and every operand here is a temporary that would then need a slot - where two
        // by-borrow arguments are what AST::CallResolver already knows how to fit
        ExprNode &str_call(
            const std::string &name,
            const TokenReference &at,
            std::vector<ExprNode *> operands
        );

        Bundle &_bundle;
        Collector &_collector;

        Module *_current_module = nullptr;
        File *_current_file = nullptr;

        bool _changed = false;
    };
};

#endif
