#include "AST/ASTCast.h"

#include "AST/ASTArgumentFit.h"
#include "AST/ASTConformance.h"
#include "AST/ASTNode.h"
#include "AST/ASTNullability.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"

#include <fmt/core.h>
#include <cassert>

namespace AST
{
    CastLookup cast_plan_for(const ExprNode &from_expr, const ValueType &to)
    {
        const ValueType from = from_expr.result_type();

        // void is "no information" to is_undetermined_type, and a written `as void` is a real
        // refusal rather than a not-yet. asked first so it cannot stall the fixpoint
        if (from.is_void() || to.is_void()) {
            return CastLookup::refused(fmt::format(
                "'{}' cannot be read as a '{}'",
                from.get_type_desciption(), to.get_type_desciption()));
        }

        if (is_undetermined_type(from) || is_undetermined_type(to)) {
            return CastLookup::pending();
        }

        if (ValueType::make_mutable(from) == ValueType::make_mutable(to)) {
            return CastLookup::ok(CastKind::t_identity);
        }

        // any written pointer-to-pointer, including a different pointee and the T&($p:$) promotion.
        // the existing Type(...) form already accepts these; unsafe is narrowing_promotes_raw_storage
        if (from.is_pointer() && to.is_pointer()) {
            return CastLookup::ok(CastKind::t_pointer);
        }

        if (from.is_primitive() && to.is_primitive()) {
            return CastLookup::ok(CastKind::t_numeric);
        }

        if (arrival_wraps_optional(from, to)) {
            return CastLookup::ok(CastKind::t_optional_wrap);
        }

        if (to.is_interface() && from.is_class()) {
            const std::string why = interface_erasure_refusal(from, to);

            if (why.empty()) {
                return CastLookup::ok(CastKind::t_interface);
            }

            return CastLookup::refused(why);
        }

        // the same peel CallResolver uses at an argument. a second copy of it is the
        // recurring bug: `$cb as Window` on a const Buffer& has to agree with span($cb)
        if (FunctionDeclNode *decl = implicit_conversion_for(from, &from_expr, to)) {
            return CastLookup::ok(CastKind::t_declared, decl);
        }

        // the two operations that look like a cast and are not. named so the refusal points at the
        // spelling that actually unwraps / upgrades, rather than at a missing conversion
        if (!from.is_pointer() && from.is_nullable() && !to.is_nullable()) {
            return CastLookup::refused(fmt::format(
                "'{}' does not narrow through 'as' - unwrap it with guard, ?? or ?->",
                from.get_type_desciption()));
        }

        if (from.is_weak() && !to.is_weak()) {
            return CastLookup::refused(fmt::format(
                "'{}' does not become a '{}' through 'as' - write 'strong(...)'",
                from.get_type_desciption(), to.get_type_desciption()));
        }

        return CastLookup::refused(fmt::format(
            "'{}' cannot be read as a '{}'",
            from.get_type_desciption(), to.get_type_desciption()));
    }

    ExprNode *emit_declared_conversion(
        NodeCollection &nodes,
        ExprNode *arg,
        FunctionDeclNode *conversion,
        const ValueType &destination,
        const TokenReference &at
    )
    {
        assert(conversion != nullptr && "emit_declared_conversion needs the declaration");
        assert(arg != nullptr && "emit_declared_conversion needs the operand");

        // inbound is a static of the destination: no receiver, one by-value argument.
        // implicit_conversion_for already peeled a T& source; the argument is a value
        // edge and PointerAdjuster inserts the deref. no second fit
        if (!conversion->has_receiver()) {
            auto &conversion_call = nodes.emplace_back<FunctionCallExprNode>(
                at, std::vector<ExprNode *>{ arg });

            conversion_call.decl = conversion;
            conversion_call.static_owner = implicit_conversion_target(destination);
            conversion_call.settlement = CallSettlement::t_settled;

            return &conversion_call;
        }

        // outbound: the conversion's `$this` is a borrow, so the receiver is addressed -
        // **unless the argument already is one**. AST::receiver_for_member_call owns that
        // rule for every synthesized member call
        auto &conversion_call = nodes.emplace_back<FunctionCallExprNode>(
            at, std::vector<ExprNode *>{ receiver_for_member_call(nodes, arg) });

        conversion_call.decl = conversion;
        conversion_call.settlement = CallSettlement::t_settled;

        return &conversion_call;
    }
};
