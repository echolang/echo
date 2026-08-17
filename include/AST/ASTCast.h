#ifndef ASTCAST_H
#define ASTCAST_H

#pragma once

#include "AST/ASTValueType.h"

#include "Token.h"

#include <string>

namespace AST
{
    class ExprNode;
    class FunctionDeclNode;
    class NodeCollection;

    // **what a written `$x as T` (or `T(...)`) means.** the sole answer, modelled on
    // AST::unwrap_plan_for: three results and not a bool, reports nothing itself.
    //
    // asked only of an explicit TypeCastNode. an implicit one the compiler inserted is
    // TypeChecker::implicit_conversion_is_legal / AST::argument_fit - two questions
    enum class CastKind
    {
        // same type after dropping top-level const. a no-op
        t_identity,

        // primitive to primitive. TypeLowering::coerce_value already lowers these
        t_numeric,

        // T -> T?, including when T converts to the payload. AST::arrival_wraps_optional
        t_optional_wrap,

        // class handle to a storable interface. AST::interface_erasure_refusal accepted it
        t_interface,

        // an #[implicit] conversion. plan.decl is the method; AST::emit_declared_conversion
        // is what turns the TypeCastNode into a call
        t_declared,

        // pointer to pointer: a reinterpret, a borrow widening, or the T&($p:$) promotion.
        // AST::narrowing_promotes_raw_storage stays the unsafe question
        t_pointer,
    };

    struct CastPlan
    {
        CastKind kind = CastKind::t_identity;
        FunctionDeclNode *decl = nullptr;
    };

    struct CastLookup
    {
        enum class Result
        {
            t_ok,
            t_pending,
            t_refused,
        };

        Result result = Result::t_pending;
        CastPlan plan;
        std::string refusal;

        static CastLookup ok(CastKind kind, FunctionDeclNode *decl = nullptr)
        {
            CastLookup out;
            out.result = Result::t_ok;
            out.plan.kind = kind;
            out.plan.decl = decl;
            return out;
        }

        static CastLookup pending()
        {
            return CastLookup{};
        }

        static CastLookup refused(std::string why)
        {
            CastLookup out;
            out.result = Result::t_refused;
            out.refusal = std::move(why);
            return out;
        }
    };

    // **the sole answer to "what does this written cast mean".** arm order is the content - see
    // the .cpp. asked of the operand, not of a pair of types: the declared arm is
    // AST::implicit_conversion_for, and that peel needs the expression. a new built-in kind is a
    // new arm here and a reader that switches on it
    CastLookup cast_plan_for(const ExprNode &from, const ValueType &to);

    // **the one mint for an #[implicit] conversion call.** CallResolver::convert_if_wanted and
    // AST::CastResolution both retrieve a declaration and then ask this - inbound static vs
    // outbound method, receiver_for_member_call, static_owner, t_settled. a second copy of that
    // mint is the recurring bug
    ExprNode *emit_declared_conversion(
        NodeCollection &nodes,
        ExprNode *arg,
        FunctionDeclNode *conversion,
        const ValueType &destination,
        const TokenReference &at
    );
};

#endif
