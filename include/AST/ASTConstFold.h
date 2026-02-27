#ifndef ASTCONSTFOLD_H
#define ASTCONSTFOLD_H

#pragma once

#include "AST/ASTValueType.h"

#include <cstdint>
#include <string>

namespace AST
{
    class ExprNode;

    // **the sole answer to "what does this expression fold to before codegen".**
    //
    // three results rather than an optional, for AST::iteration_plan_for's reason: `t_pending` is what
    // an expression whose calls the fixpoint has not settled yet must get, and it has to be
    // distinguishable from a refusal - otherwise the first round's guess becomes the diagnostic, once
    // per round, at a token nothing was wrong with.
    //
    // it reports nothing itself: it has no CodeRef and each of its callers has one. the split
    // AST::interface_erasure_refusal makes, and for the same reason
    struct ConstFoldResult
    {
        enum class Result
        {
            t_folded,

            // ask again next round: a call in here is not settled, or its type argument is not bound
            t_pending,

            // no round will ever answer it
            t_refused,
        };

        Result result = Result::t_pending;

        // the type the value was folded **at**, carried rather than re-derived: the comparison arms had
        // to read it anyway to know whether `<` is signed, and every caller has to be able to insist on
        // a type of its own
        ValueType type = ValueType::make_unknown();

        // the value, as bits read through `type`. **unsigned storage on purpose**: a `uint64` above
        // INT64_MAX is a legal Echo literal and has to survive being carried, and the sign is the
        // type's business rather than the storage's - the split llvm::APInt makes, minus the width
        // `type` already carries.
        //
        // **the invariant every arm maintains**: a signed value is stored sign-extended to 64 bits, an
        // unsigned one zero-extended. so `as_signed()` is a cast and never a shift, and two values of
        // one type compare correctly by comparing these two fields
        uint64_t bits = 0;

        // a whole sentence, phrased for whoever wrote the expression. empty unless t_refused
        std::string refusal;

        inline bool is_folded() const { return result == Result::t_folded; }
        inline bool is_bool() const { return is_folded() && type.is_boolean_type(); }

        // truthiness, for the one caller that wants a branch rather than a value
        inline bool as_bool() const { return bits != 0; }

        inline int64_t as_signed() const { return static_cast<int64_t>(bits); }

        static ConstFoldResult folded(const ValueType &type, uint64_t bits);
        static ConstFoldResult pending();
        static ConstFoldResult refused(std::string why);
    };

    // **this is not an evaluator, and this is the line.** notes/constants.md states that a constant is
    // its *expression*, cloned per use site, and that there is no evaluator and should not be. that is
    // unchanged: this is never asked what a constant denotes, and it *cannot* be - AST::ConstantExpander
    // runs before the monomorphizer's fixpoint, so an AST::ConstRefExprNode is already gone by the time
    // anything calls this, and there is no arm for one. a constant still has no value; it has an
    // expression, and if that expression is foldable then so is every other expression of that shape.
    //
    // what this owns is narrower: **does a marked expression decide itself, and to what**. it is asked
    // by AST::ConstFolding for a `const if`'s condition and for a `const(...)`, and by
    // ExprCodegen::gen_type_query_builtin for the two AST-fact builtins - which used to fold them
    // itself, two spellings of one fact held in step by nothing. so this is a net *reduction* in owners
    // rather than a second one to keep in step.
    //
    // side-effect free and independent of any pass's state, which is what lets codegen and a fixpoint
    // round ask the same question and rely on the same answer
    ConstFoldResult const_fold(const ExprNode *expr);
};

#endif
