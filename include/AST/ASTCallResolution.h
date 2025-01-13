#ifndef ASTCALLRESOLUTION_H
#define ASTCALLRESOLUTION_H

#pragma once

#include "AST/ASTCodeRef.h"
#include "AST/ASTNode.h"
#include "AST/ExprNode.h"

#include <vector>

namespace AST
{
    class Collector;
    class FunctionDeclNode;

    // the whole life of a call: which declaration it names, and how its arguments reach that
    // declaration's parameters
    //
    // **one implementation, because there is one question.** the parser and the monomorphizer are two
    // askers of it at two moments, and the moment is the only difference between them. they used to be
    // two copies - Parser::resolve_funccall's coercion loop and Monomorphizer::insert_argument_casts -
    // and the copies disagreed about the case that matters: the parser's ran for a *concrete* callee
    // whose argument types were not necessarily known yet, and coerced against a type that said
    // nothing, while the monomorphizer's ran only for a generic one and always after substitution. so
    // `f($q)` with `$q` initialized from a generic constructor got a TypeCastNode where an
    // AddrOfExprNode belonged, and the identical non-generic program did not
    //
    // **the moment is a state on the node**, AST::CallSettlement, not something a pass remembers.
    // that is what lets the fixpoint re-enter this: it already re-derives the variable types a call is
    // waiting on, so settling the call there needs no second mechanism and no queue - only the ability
    // to ask again, and to answer "not yet"
    class CallResolver
    {
    public:
        enum class Result
        {
            // the call is finished: a declaration is chosen and its arguments are fitted
            t_settled,

            // nothing is wrong, but the types known right now cannot finish it. ask again after the
            // round that answers them
            t_pending,

            // the name is declared nowhere this call can see. **not reported here**: "no such
            // function" and "no such member" are different errors at different tokens, so each caller
            // keeps its own wording. nothing to retry either - a later round declares no new names
            t_unknown_name,

            // reported. the caller stops building on this call
            t_failed,
        };

        explicit CallResolver(Collector &collector) : _collector(collector) {};
        ~CallResolver() {};

        // one attempt at taking `call` as far as the types known right now allow: choose the
        // declaration if it has none, then fit the arguments to it
        //
        // `report` turns the deferrable diagnostic on. while the fixpoint may still answer what an
        // argument is, an undecidable call is a not-yet rather than an error - so the parser asks
        // with `report = false` and the finalizing sweep, after the fixpoint has stopped, asks with
        // `report = true`. an *ambiguous* or unmatchable call is final either way and is reported the
        // first time it is seen, at the token the user wrote
        //
        // a generic callee answers t_pending: instantiating it is the monomorphizer's, and it happens
        // between the two halves of this
        Result settle(FunctionCallExprNode &call, NodeCollection &nodes, const CodeRef &at, bool report);

        // fit each argument to its parameter: an implicit address-of where the parameter is a borrow
        // and the argument a place, an implicit cast for whatever is left over. public because the
        // finalizing sweep coerces a call it is about to give up on, so the tree it hands the later
        // passes has the shape they expect
        //
        // **not idempotent** - it wraps, and wrapping twice is wrong. AST::CallSettlement is what
        // guarantees it runs once
        void coerce_arguments(FunctionCallExprNode &call, NodeCollection &nodes);

        // true when every argument's type is known, so a decision made about them is final rather
        // than premature
        static bool arguments_are_determined(const FunctionCallExprNode &call);

    private:
        Collector &_collector;

        // the candidates the call's name denotes, re-derived rather than stored: from
        // `lookup_namespace` for a free call, from the receiver's type for a member one. empty when
        // the name is declared nowhere, which `settle` hands back as t_unknown_name for the caller
        // to word - "no such function" and "no such member" are different errors, both located at
        // the token their caller read
        std::vector<FunctionDeclNode *> candidates_for(const FunctionCallExprNode &call) const;

        // run the overload match and record its answer on the call. `candidates` is passed in
        // because `settle` has just derived the set for its own empty-set test
        Result choose_declaration(
            FunctionCallExprNode &call,
            const std::vector<FunctionDeclNode *> &candidates,
            const CodeRef &at,
            bool report);
    };
};

#endif
