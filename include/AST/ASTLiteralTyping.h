#ifndef ASTLITERALTYPING_H
#define ASTLITERALTYPING_H

#pragma once

#include "AST/ASTValueType.h"

#include <optional>
#include <string>

namespace AST
{
    class Collector;
    class ExprNode;
    class File;
    class LiteralPrimitiveExprNode;
    class Module;
    class NodeCollection;
    struct CodeRef;
    struct Operator;

    // **is this literal's type a default nobody chose?**
    //
    // the question every rule below rests on, and the one the tree could not answer at all until
    // LiteralPrimitiveExprNode stopped stamping its guess into `expected_primitive_type`. a literal is
    // the operand with *no* opinion - it is the destination, the typed neighbour or the bound type
    // parameter that has one - and without this a defaulted `int32` was indistinguishable from an
    // `int32` somebody asked for, so six positions deferred to the wrong side.
    //
    // a literal whose type nobody chose, written bools included. a `true` never engages
    // `expected_primitive_type` (`type_bool_literal_at` is `t_unchanged` at a bool destination),
    // so this answers true for one; family checks decide whether that opinion matters. `1` at a
    // bool destination is a *typed* bool, because the destination chose it
    bool is_untyped_literal(const ExprNode *expr);

    // the token a literal was written at, for a caller that has to point at one. asserts rather than
    // answers an empty token: every site that reports about a literal has already established it is
    // holding one, and a silent fallback would put the caret on whatever the parser last touched
    const TokenReference &literal_token_of(const ExprNode *expr);

    // which channel a refusal belongs on. the caller reports - this file has no CodeRef and every one
    // of its six askers does, the split AST::interface_erasure_refusal and AST::ArrayLiteralLookup
    // both make - so the kind travels with the sentence rather than being re-derived from it
    enum class LiteralRefusal
    {
        t_overflow,             // Issue::IntegerOverflow
        t_underflow,            // Issue::IntegerUnderflow
        t_invalid_conversion,   // Issue::InvalidTypeConversion
    };

    // **the sole answer to "what does this literal become at this destination, and why not".**
    //
    // it used to be `autocast_literal_int` / `autocast_literal_float` / `parse_literal_boolean`, three
    // Parser::Payload-shaped helpers inside src/Parser/ExprParser.cpp - which is why the four positions
    // that are not the parser (a binary operand, a call argument, a bound type parameter, a substituted
    // declaration) each grew their own answer or none at all. Moving it here is the whole of what makes
    // one rule serve all six.
    //
    // three results and not an optional, for AST::ArrayLiteralLookup's reason: "the destination cannot
    // decide this" is not a refusal, and a caller that read it as one would report against a hint that
    // was never meant to be one
    struct LiteralTyping
    {
        enum class Result
        {
            // the destination said nothing about it - not a primitive. `node` is the literal, untouched.
            // a number at a `bool` is not this: 0 and 1 type, anything else refuses
            t_unchanged,

            // `node` is the literal at the destination's type. it may be a **different node** - an
            // integer literal at a float destination becomes a float one
            t_typed,

            // `refusal` is a whole sentence, on the channel `refusal_kind` names
            t_refused,
        };

        Result result = Result::t_unchanged;

        ExprNode *node = nullptr;

        LiteralRefusal refusal_kind = LiteralRefusal::t_invalid_conversion;
        std::string refusal;

        // a narrowing that survives but not intact - float64 -> float32 where the round trip moves.
        // beside a `t_typed` and never beside a refusal, because the value is still usable
        std::optional<std::string> warning;
    };

    // `destination` is the type as written at the position the literal sits in. A destination that
    // cannot decide a literal is answered `t_unchanged` rather than refused. this is the rule: it
    // types at any concrete primitive, `bool` included (0 and 1 become true and false).
    // AST::can_type_a_literal is the *parse-time operand-hint* filter only, and the two differ by
    // exactly that one case - a shunting yard must not hand `bool` to an operand it has not read
    // the operator for
    LiteralTyping type_literal_at(ExprNode *literal, const ValueType &destination, NodeCollection &nodes);

    // **which channel a refusal goes out on, written once.** four askers report one of these, and the
    // mapping from `refusal_kind` to an Issue class is exactly the kind of thing that drifts when it is
    // spelled per caller - one of them ends up reporting an overflow as a conversion and the badge in
    // 248 goldens stops meaning anything
    void report_literal_refusal(Collector &collector, const CodeRef &at, LiteralRefusal kind, const std::string &sentence);
    void report_literal_refusal(Collector &collector, const CodeRef &at, const LiteralTyping &typing);

    // the warning sibling: a float64→float32 narrowing where the round trip moves. every asker
    // that accepts `t_typed` goes through this, or a call / a bound type parameter / a binary
    // operand would drop the same sentence the parser reports
    void report_literal_warning(Collector &collector, const CodeRef &at, const std::optional<std::string> &warning);
    void report_literal_warning(Collector &collector, const CodeRef &at, const LiteralTyping &typing);

    // the caller's CodeRef, re-pointed at the literal's own token. every asker holds one for the
    // statement or the call it is walking and none holds one for the operand, so the caret would
    // otherwise land on the wrong thing in three places out of four
    CodeRef code_ref_at_literal(const CodeRef &within, const ExprNode *literal);

    // **what two operands of one binary expression agree on**, and the one place the answer is written.
    //
    // the parser and AST::OperatorRewriter both reconcile a binary expression's operands, and both used
    // to spell the same policy - "wrap whichever side's primitive is not the common type in a cast".
    // That policy has no notion of which side *knows* what it is, so `uint32 $u = ...; $u / 2` cast the
    // **variable** down to meet an `int32` the literal had defaulted to, and every sign-sensitive
    // operator downstream read a negative number.
    //
    // the rule, in order:
    //
    //  1. exactly one operand is an untyped literal and both are in the same numeric family - the
    //     literal is *retyped* at the typed operand's type. no cast node at all, and the typed operand
    //     is left alone, which is the whole fix
    //  2. otherwise AST::common_numeric_type as before, cast on the side that differs. that keeps the
    //     cross-family step (`$i + 2.5` is float arithmetic) and every widening between two typed
    //     operands exactly as it was
    //  3. where (2) would reconcile a typed operand into something that **reinterprets** it - a
    //     same-width signedness change - and the other side is a literal that simply did not fit, it is
    //     refused instead. silently reading `$u` as negative is the bug; widening it is not
    //
    // reports nothing, mints nothing on a refusal. `refused_operand` is the literal to point at
    struct BinaryReconciliation
    {
        enum class Result
        {
            t_unchanged,
            t_reconciled,
            t_refused,
        };

        Result result = Result::t_unchanged;

        ExprNode *lhs = nullptr;
        ExprNode *rhs = nullptr;

        std::string refusal;
        LiteralRefusal refusal_kind = LiteralRefusal::t_invalid_conversion;
        LiteralPrimitiveExprNode *refused_operand = nullptr;

        // a float64→float32 narrowing that survived. beside `t_reconciled`, pointed at the
        // operand that was retyped - the callers report through report_binary_reconciliation
        std::optional<std::string> warning;
        ExprNode *warned_operand = nullptr;
    };

    BinaryReconciliation reconcile_binary_operands(
        const Operator *op, ExprNode *lhs, ExprNode *rhs, NodeCollection &nodes);

    // the two binary sites' reporting, written once: a warning if the retype moved, a refusal
    // on the channel `refusal_kind` names. module and file come from the caller; the caret
    // is the operand's own token
    void report_binary_reconciliation(
        Collector &collector,
        const Module *module,
        const File *file,
        const BinaryReconciliation &reconciled);
};

#endif
