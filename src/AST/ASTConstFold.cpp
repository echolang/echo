#include "AST/ASTConstFold.h"

#include "AST/ASTBuiltin.h"
#include "AST/ASTCopy.h"
#include "AST/ASTDestruction.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ConstExprNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/OperatorNode.h"
#include "AST/TypeCastNode.h"

#include <fmt/core.h>

#include <cerrno>
#include <cstdlib>
#include <optional>

using AST::ConstFoldResult;

AST::ConstFoldResult AST::ConstFoldResult::folded(const AST::ValueType &type, uint64_t bits)
{
    ConstFoldResult out;
    out.result = Result::t_folded;
    out.type = type;
    out.bits = bits;
    return out;
}

AST::ConstFoldResult AST::ConstFoldResult::pending()
{
    return ConstFoldResult{};
}

AST::ConstFoldResult AST::ConstFoldResult::refused(std::string why)
{
    ConstFoldResult out;
    out.result = Result::t_refused;
    out.refusal = std::move(why);
    return out;
}

namespace
{
    typedef ConstFoldResult::Result Result;

    ConstFoldResult fold_bool(bool value)
    {
        return ConstFoldResult::folded(AST::ValueType(AST::ValueTypePrimitive::t_bool), value ? 1 : 0);
    }

    // **an integer type this can compute in at all.** a nullable one is refused rather than unwrapped:
    // a `T?` value carries a presence flag beside the number, so there is no single set of bits to fold,
    // and AST::optional_operand_of is the owner of what unwraps one
    //
    // "carries a presence flag beside the value" *is* ValueType::is_wrapped_optional, which is what
    // binary_has_builtin_meaning's arithmetic gate reads - so the folder and the lowering refuse the
    // same operand rather than two hand-spelled versions of it
    bool is_foldable_integer(const AST::ValueType &type)
    {
        return type.is_integer_type() && !type.is_wrapped_optional();
    }

    bool is_foldable_bool(const AST::ValueType &type)
    {
        return type.is_boolean_type() && !type.is_wrapped_optional();
    }

    // does `value`, read through `type`, fit in `type`'s width? the invariant ConstFoldResult::bits
    // documents, checked - a sign-extended int8 of -1 is 0xFF..FF and fits, and 0x1FF does not
    bool fits(const AST::ValueType &type, uint64_t bits)
    {
        const AST::IntegerSize size = AST::get_integer_size(type.get_primitive_type());

        if (!size.is_signed) {
            return bits <= size.get_max_positive_value();
        }

        const int64_t value = static_cast<int64_t>(bits);

        return value <= static_cast<int64_t>(size.get_max_positive_value())
            && value >= size.get_max_negative_value();
    }

    // a negative literal is folded through the same arm a written negation would take, so the width
    // check exists once. declared here because the literal arm is above it and reads it
    ConstFoldResult fold_negation(const AST::ValueType &type, uint64_t magnitude);

    // the literal's magnitude, plus whether it was written negative. **the sign is part of the token**:
    // the expression parser folds a leading `-` into the literal rather than leaving a UnaryExprNode, so
    // `int8 $s = -128;` is one node whose text is "-128".
    //
    // **not the LiteralIntExprNode accessors**: those are std::stoll and friends, which *throw* on a
    // value that does not fit, and a literal too wide for its type is something to refuse with a
    // sentence rather than to crash on
    bool read_digits(const std::string &text, uint64_t &magnitude, bool &negative)
    {
        if (text.empty()) {
            return false;
        }

        negative = text[0] == '-';

        // strtoull would accept the sign itself and negate in unsigned space, which turns "-1" into the
        // widest unsigned value and hides the very thing the caller has to branch on
        const char *digits = text.c_str() + (negative || text[0] == '+' ? 1 : 0);

        if (*digits == '\0') {
            return false;
        }

        errno = 0;

        char *end = nullptr;
        const unsigned long long parsed = std::strtoull(digits, &end, 10);

        if (errno == ERANGE || end == nullptr || *end != '\0') {
            return false;
        }

        magnitude = static_cast<uint64_t>(parsed);

        return true;
    }

    ConstFoldResult fold_int_literal(const AST::LiteralIntExprNode &literal)
    {
        const AST::ValueType type = literal.result_type();

        if (!is_foldable_integer(type)) {
            return ConstFoldResult::refused(fmt::format(
                "a '{}' literal is not something the compiler can fold to a value.",
                type.get_type_desciption()));
        }

        uint64_t magnitude = 0;
        bool negative = false;

        if (!read_digits(literal.effective_token_literal_value(), magnitude, negative)) {
            return ConstFoldResult::refused(fmt::format(
                "'{}' is not a decimal integer this can fold - only base-ten digits are read here.",
                literal.effective_token_literal_value()));
        }

        if (negative) {
            return fold_negation(type, magnitude);
        }

        if (magnitude > AST::get_integer_size(type.get_primitive_type()).get_max_positive_value()) {
            return ConstFoldResult::refused(fmt::format(
                "{} does not fit in a '{}'.", magnitude, type.get_type_desciption()));
        }

        return ConstFoldResult::folded(type, magnitude);
    }

    ConstFoldResult fold_negation(const AST::ValueType &type, uint64_t magnitude)
    {
        const AST::IntegerSize size = AST::get_integer_size(type.get_primitive_type());

        if (!size.is_signed) {
            return ConstFoldResult::refused(fmt::format(
                "'-' has no meaning on a '{}', which cannot hold a negative value.",
                type.get_type_desciption()));
        }

        // the magnitude of the width's minimum is one more than its maximum, so this admits exactly
        // `-128` for an int8 and refuses `-129`
        if (magnitude > static_cast<uint64_t>(size.get_max_positive_value()) + 1) {
            return ConstFoldResult::refused(fmt::format(
                "-{} does not fit in a '{}'.", magnitude, type.get_type_desciption()));
        }

        // negated in unsigned space and then reinterpreted, because negating the int64 minimum is
        // itself signed overflow - IntegerSize::get_max_negative_value does the same thing for the
        // same reason. the result is sign-extended to 64 bits, which is ConstFoldResult's invariant
        return ConstFoldResult::folded(type, ~magnitude + 1);
    }

    // **overflow is a refusal, not a wrap.** ExprCodegen::gen_binary_expr emits the target's wrapping
    // arithmetic, and this does not reimplement it: it computes in the operand's width and declines the
    // moment the result would not fit. so `const(255 + 1)` on a uint8 is a located error where the same
    // expression at runtime wraps to 0 - a divergence, deliberately, in the one direction that compiles
    // nothing. a `const` marker means the compiler stands behind the value, and a wrapped one is a value
    // nobody asked for
    ConstFoldResult fold_arithmetic(
        Token::Type op, const std::string &spelling, const AST::ValueType &type,
        uint64_t lhs, uint64_t rhs)
    {
        const AST::IntegerSize size = AST::get_integer_size(type.get_primitive_type());

        const auto checked = [&](uint64_t value) {
            if (!fits(type, value)) {
                return ConstFoldResult::refused(fmt::format(
                    "this does not fit in a '{}' - a 'const' value is refused rather than wrapped.",
                    type.get_type_desciption()));
            }

            return ConstFoldResult::folded(type, value);
        };

        switch (op) {
            case Token::Type::t_op_add:
            case Token::Type::t_op_sub:
            case Token::Type::t_op_mul: {
                // **in 64 bits of the operand's own signedness**, which is what the two spellings differ
                // in and nothing else - so it is one generic lambda over the width, fold_comparison's
                // `ordered` shape. nullopt is the 64-bit overflow; `checked` is what narrows to the
                // declared type afterwards
                const auto arithmetic = [&](auto a, auto b) -> std::optional<uint64_t> {
                    decltype(a) out = 0;

                    const bool overflowed = op == Token::Type::t_op_add ? __builtin_add_overflow(a, b, &out)
                        : op == Token::Type::t_op_sub ? __builtin_sub_overflow(a, b, &out)
                                                      : __builtin_mul_overflow(a, b, &out);

                    if (overflowed) {
                        return std::nullopt;
                    }

                    return static_cast<uint64_t>(out);
                };

                const std::optional<uint64_t> out = size.is_signed
                    ? arithmetic(static_cast<int64_t>(lhs), static_cast<int64_t>(rhs))
                    : arithmetic(lhs, rhs);

                if (!out.has_value()) {
                    return ConstFoldResult::refused(fmt::format(
                        "this overflows a '{}' - a 'const' value is refused rather than wrapped.",
                        type.get_type_desciption()));
                }

                return checked(*out);
            }

            case Token::Type::t_op_div:
            case Token::Type::t_op_mod: {
                if (rhs == 0) {
                    return ConstFoldResult::refused(
                        "this divides by zero. at runtime that is undefined; here it is simply refused.");
                }

                if (size.is_signed) {
                    const int64_t a = static_cast<int64_t>(lhs);
                    const int64_t b = static_cast<int64_t>(rhs);

                    // the one signed division that overflows: the width's minimum over -1
                    if (b == -1 && a == size.get_max_negative_value()) {
                        return ConstFoldResult::refused(fmt::format(
                            "this overflows a '{}' - a 'const' value is refused rather than wrapped.",
                            type.get_type_desciption()));
                    }

                    return checked(static_cast<uint64_t>(
                        op == Token::Type::t_op_div ? a / b : a % b));
                }

                return checked(op == Token::Type::t_op_div ? lhs / rhs : lhs % rhs);
            }

            // **the three that cannot leave the type they fold at**, which is the whole of why they need
            // none of the overflow machinery above: both operands already fit, and every bit of the answer
            // comes from one of them. That holds for a signed operand too, because a ConstFoldResult is
            // sign-extended to 64 bits - so `&`, `|` and `^` over two sign-extended values produce the
            // sign-extended answer with nothing to correct.
            //
            // `checked` stays anyway rather than returning `folded` directly, so the arm makes no claim of
            // its own about what fits - one answer to that question, in one lambda
            case Token::Type::t_and:
                return checked(lhs & rhs);
            case Token::Type::t_or:
                return checked(lhs | rhs);
            case Token::Type::t_xor:
                return checked(lhs ^ rhs);

            // **the two shifts, and a shift count is a refusal before it is an answer** - asked of
            // AST::shift_count_refusal, which TypeChecker asks too so that `const(1 << 32)` and the
            // plain `1 << 32` beside it get the same sentence rather than an error and a silence.
            //
            // `type` is the *left* operand's here whatever the count was written as, because
            // AST::binary_reconciles_operands kept the reconciliation above from touching a shift
            case Token::Type::t_op_shl:
            case Token::Type::t_op_shr: {
                if (auto refusal = shift_count_refusal(type, rhs)) {
                    return ConstFoldResult::refused(*refusal);
                }

                if (op == Token::Type::t_op_shl) {
                    // computed in unsigned space, where the wrap is *defined* - a signed left shift past
                    // the sign bit is C++ undefined behaviour, and this file must not have any. `checked`
                    // is then what refuses the ones that left the type, so `1 << 7` on an int8 is a
                    // located error exactly as `1 * 128` already is
                    return checked(lhs << rhs);
                }

                // **the one bitwise arm that is not sign-agnostic**, mirroring ExprCodegen's `ashr`/`lshr`
                // split and reading the same reconciled signedness it does. an arithmetic shift of a
                // sign-extended value stays sign-extended, so nothing has to be repaired afterwards
                if (size.is_signed) {
                    return checked(static_cast<uint64_t>(static_cast<int64_t>(lhs) >> rhs));
                }

                return checked(lhs >> rhs);
            }

            // **`**` is deliberately not folded.** ExprCodegen lowers it by casting both operands to
            // double, calling llvm.pow and casting back to the operand type - so an integer answer worked
            // out here would differ from the emitted one for any input where that round trip loses
            // precision, and it would differ *silently*. the round trip is the reason, not the width it
            // lands in.
            //
            // the bitwise five above are folded because codegen lowers all five. that pairing is the rule
            // rather than a coincidence: a symbol folded here and thrown on there makes a `const if` and
            // the `if` beside it take different arms, so the two halves move together or not at all
            default:
                return ConstFoldResult::refused(fmt::format(
                    "'{}' is not something the compiler folds. `+ - * / % & | ^ << >>`, the comparisons "
                    "and `&&`/`||` are.", spelling));
        }
    }

    ConstFoldResult fold_comparison(
        Token::Type op, const std::string &spelling, const AST::ValueType &type,
        uint64_t lhs, uint64_t rhs)
    {
        if (op == Token::Type::t_logical_eq) {
            return fold_bool(lhs == rhs);
        }

        if (op == Token::Type::t_logical_neq) {
            return fold_bool(lhs != rhs);
        }

        if (!is_foldable_integer(type)) {
            return ConstFoldResult::refused(fmt::format(
                "'{}' has no order over a '{}'.", spelling, type.get_type_desciption()));
        }

        // **read through the type's own signedness**, and through the same accessor
        // ExprCodegen::gen_binary_expr reads to pick between `icmp ult` and `icmp slt`. the type is the
        // reconciled one both sides get from AST::binary_operation_type, so a `const if` and the `if`
        // beside it cannot take different arms - which they did, for any unsigned value above the
        // signed maximum, while that arm emitted CreateICmpS* unconditionally
        const bool is_signed = type.is_signed_integer();

        const auto ordered = [&](auto a, auto b) {
            switch (op) {
                case Token::Type::t_open_angle:   return a < b;
                case Token::Type::t_close_angle:  return a > b;
                case Token::Type::t_logical_leq:  return a <= b;
                case Token::Type::t_logical_geq:  return a >= b;
                default:                          return false;
            }
        };

        return fold_bool(is_signed
            ? ordered(static_cast<int64_t>(lhs), static_cast<int64_t>(rhs))
            : ordered(lhs, rhs));
    }

    ConstFoldResult fold_builtin_call(const AST::FunctionCallExprNode &call)
    {
        // no declaration yet, or one the monomorphizer has not replaced with an instance. both are
        // not-yets: the fixpoint has more rounds, and settle_calls or instantiate_generic_calls is what
        // answers them
        if (call.decl == nullptr || call.decl->is_generic()) {
            return ConstFoldResult::pending();
        }

        if (!call.decl->is_builtin()) {
            return ConstFoldResult::refused(fmt::format(
                "'{}' is an ordinary function, so its result is only known when it runs.",
                call.decl->func_name()));
        }

        const AST::BuiltinKind kind = AST::builtin_kind_for(call.decl->builtin.value());

        switch (AST::builtin_foldability(kind)) {
            case AST::BuiltinFoldability::t_ast_fact:
                break;

            case AST::BuiltinFoldability::t_needs_layout:
                return ConstFoldResult::refused(fmt::format(
                    "'{}' is answered from the target's layout, which the compiler only knows once it is "
                    "emitting code - so it cannot decide a 'const' expression.",
                    call.decl->builtin.value()));

            case AST::BuiltinFoldability::t_not_a_query:
                return ConstFoldResult::refused(fmt::format(
                    "'{}' does something rather than answering something, so there is nothing to fold.",
                    call.decl->builtin.value()));
        }

        // **the guard ExprCodegen::gen_type_query_builtin has always carried, and the reason it is
        // load-bearing here too**: AST::classify_copy and AST::needs_destruction both answer "no" for an
        // unsettled type parameter, on purpose - a not-yet rather than a refusal - so folding either
        // against a `T` the monomorphizer has not bound yet is silently the wrong answer in the one
        // direction that compiles. the two callers differ only in what they do about it: codegen throws,
        // because an un-instantiated template reaching it is a compiler bug, and a fixpoint round waits
        if (call.decl->instantiation_args.size() != 1) {
            return ConstFoldResult::pending();
        }

        const AST::ValueType &subject = call.decl->instantiation_args[0];

        if (subject.is_type_param()) {
            return ConstFoldResult::pending();
        }

        switch (kind) {
            case AST::BuiltinKind::t_is_trivially_copyable:
                return fold_bool(AST::classify_copy(subject) == AST::CopyKind::t_bytes);

            case AST::BuiltinKind::t_needs_destruction:
                return fold_bool(AST::needs_destruction(subject));

            default:
                // unreachable: builtin_foldability answered t_ast_fact, and those are the two
                return ConstFoldResult::refused(fmt::format(
                    "'{}' is not a fact about a type.", call.decl->builtin.value()));
        }
    }
}

AST::ConstFoldResult AST::const_fold(const AST::ExprNode *expr)
{
    if (expr == nullptr) {
        return ConstFoldResult::pending();
    }

    switch (expr->get_node_type()) {
        case NodeType::n_literal_bool:
            return fold_bool(static_cast<const LiteralBoolExprNode *>(expr)->get_bool_value());

        case NodeType::n_literal_int:
            return fold_int_literal(*static_cast<const LiteralIntExprNode *>(expr));

        // a float folds to nothing here on purpose: owning a float comparison means owning rounding and
        // the target's precision, which ExprCodegen does, and a second answer to that is the recurring
        // bug in this codebase
        case NodeType::n_literal_float:
            return ConstFoldResult::refused(
                "a floating-point value is not something the compiler folds - its arithmetic belongs to "
                "the target rather than to the tree.");

        case NodeType::n_expr_unary: {
            const auto &unary = *static_cast<const UnaryExprNode *>(expr);
            const ConstFoldResult operand = const_fold(unary.expr);

            if (operand.result != Result::t_folded) {
                return operand;
            }

            // **`!` over a bool.** its other meaning - the presence test over a value that may be
            // absent - is deliberately *not* folded: that is a runtime question, and none of the
            // presence tests are folded on the binary side either, so folding this one would be the
            // one moment where `const if (!$maybe)` and the `if` beside it could disagree
            if (unary.token_operator.type() == Token::Type::t_exclamation) {
                if (!is_foldable_bool(operand.type)) {
                    return ConstFoldResult::refused(fmt::format(
                        "'!' folds over a bool, and this operand is a '{}'.",
                        operand.type.get_type_desciption()));
                }

                return fold_bool(operand.bits == 0);
            }

            // the parser folds unary `+` away, so `-` and `!` are the whole built-in surface here.
            // anything else is a declared operator, which is the arm below
            if (unary.token_operator.type() != Token::Type::t_op_sub) {
                return ConstFoldResult::refused(fmt::format(
                    "'{}' is not an operator the compiler folds.", unary.token_operator.value()));
            }

            if (!is_foldable_integer(operand.type)) {
                return ConstFoldResult::refused(fmt::format(
                    "'-' folds over a whole number, and this operand is a '{}'.",
                    operand.type.get_type_desciption()));
            }

            return fold_negation(operand.type, operand.bits);
        }

        case NodeType::n_expr_binary: {
            const auto &binary = *static_cast<const BinaryExprNode *>(expr);

            if (binary.op_node == nullptr || binary.op_node->op == nullptr) {
                return ConstFoldResult::pending();
            }

            // **the gate is the whole answer to "a declared overload must not silently fold".**
            // binary_has_builtin_meaning answers false for a custom symbol and for any complex operand,
            // so `operator (Point $a) < (Point $b)` reaches this refusal instead of being folded on
            // operand bits. asked with the *parse-time* operand facts, because this runs inside the
            // fixpoint and AST::PointerAdjuster has not inserted its derefs yet
            //
            // the wording covers both halves of what a false answer means, because the folder cannot
            // tell them apart and neither one is foldable: a symbol somebody declared runs, and a pair
            // the language spells no meaning for - `1 << 2` - has nothing to fold *to*. naming only
            // the first would say "here is a declared operator" about `<<`
            if (!binary_has_builtin_meaning(
                    binary.op_node->op, parse_time_operand(binary.lhs), parse_time_operand(binary.rhs))) {
                return ConstFoldResult::refused(fmt::format(
                    "'{}' has no built-in meaning for these operands, so there is nothing to fold - "
                    "a declared operator runs instead.",
                    binary.op_node->token_literal.value()));
            }

            const ConstFoldResult lhs = const_fold(binary.lhs);

            if (lhs.result != Result::t_folded) {
                return lhs;
            }

            // **short-circuit.** `false && _` is false and `true || _` is true, without folding the
            // right side - which is what codegen does, and what `false && die("no")` needs of a
            // `const if`. a pending or refused right side is not a reason to refuse the whole thing
            // once the left has already decided
            const Token::Type op_after_lhs = binary.op_node->op->type;

            if (op_after_lhs == Token::Type::t_logical_and && is_foldable_bool(lhs.type) && !lhs.as_bool()) {
                return fold_bool(false);
            }

            if (op_after_lhs == Token::Type::t_logical_or && is_foldable_bool(lhs.type) && lhs.as_bool()) {
                return fold_bool(true);
            }

            const ConstFoldResult rhs = const_fold(binary.rhs);

            if (rhs.result != Result::t_folded) {
                return rhs;
            }

            // **the operands may not agree yet, and the answer to that is not this file's.** the parser
            // reconciles what it can see - AST::reconcile_binary_operands, which
            // *retypes* a literal rather than wrapping it - but AST::ConstantExpander lands its clones
            // after that, so `const usize MAX = 100;` then `MAX > 50` arrives here as a usize beside an
            // int32 with nothing having reconciled them.
            //
            // so ask the one owner rather than guessing which side wins, and then re-check both values
            // against the type it named: widening is what reconciliation does, so nothing should fail
            // here, and if something does it is a value that never fitted
            //
            // asked directly rather than through AST::binary_operation_type, which is the same rule and
            // what ExprCodegen reads: that spelling folds "no common type" into "they already agree",
            // and this is the one caller that has to tell those apart to report the first
            // **a shift folds at its left operand's type and asks nothing of the count's**, which is the
            // same rule AST::binary_reconciles_operands gives the parser, the rewriter and codegen. This
            // is the reader where a second answer would be loudest: reconciling here would fold
            // `-16 >> 2` at the *count's* type where the count was written wider, and the emitted shift
            // would keep the sign bit the fold had already dropped
            ValueType folded_at = lhs.type;

            if (binary_reconciles_operands(binary.op_node->op) && !(lhs.type == rhs.type)) {
                const std::optional<ValueType> common = common_numeric_type(lhs.type, rhs.type);

                if (!common.has_value()) {
                    return ConstFoldResult::refused(fmt::format(
                        "a '{}' and a '{}' have no common type, so there is nothing to fold them at.",
                        lhs.type.get_type_desciption(), rhs.type.get_type_desciption()));
                }

                folded_at = *common;

                if (!is_foldable_integer(folded_at) || !fits(folded_at, lhs.bits) || !fits(folded_at, rhs.bits)) {
                    return ConstFoldResult::refused(fmt::format(
                        "a '{}' and a '{}' reconcile to '{}', which cannot hold both values.",
                        lhs.type.get_type_desciption(), rhs.type.get_type_desciption(),
                        folded_at.get_type_desciption()));
                }
            }

            const Token::Type op = binary.op_node->op->type;
            const std::string &spelling = binary.op_node->op->spelling;

            if (op == Token::Type::t_logical_and || op == Token::Type::t_logical_or) {
                if (!is_foldable_bool(folded_at)) {
                    return ConstFoldResult::refused(fmt::format(
                        "'{}' joins two 'bool's, and these are '{}'.",
                        spelling, folded_at.get_type_desciption()));
                }

                return fold_bool(op == Token::Type::t_logical_and
                    ? (lhs.as_bool() && rhs.as_bool())
                    : (lhs.as_bool() || rhs.as_bool()));
            }

            switch (op) {
                case Token::Type::t_logical_eq:
                case Token::Type::t_logical_neq:
                case Token::Type::t_open_angle:
                case Token::Type::t_close_angle:
                case Token::Type::t_logical_leq:
                case Token::Type::t_logical_geq:
                    return fold_comparison(op, spelling, folded_at, lhs.bits, rhs.bits);

                default:
                    break;
            }

            if (!is_foldable_integer(folded_at)) {
                return ConstFoldResult::refused(fmt::format(
                    "'{}' folds over whole numbers, and these are '{}'.",
                    spelling, folded_at.get_type_desciption()));
            }

            return fold_arithmetic(op, spelling, folded_at, lhs.bits, rhs.bits);
        }

        case NodeType::n_expr_call:
            return fold_builtin_call(*static_cast<const FunctionCallExprNode *>(expr));

        // **transparent, so nested marks cost nothing.** `const if (const(A) && B)` composes, and so does a
        // `const(...)` that AST::ConstFolding has not reached yet - which is what keeps the pass free of any
        // ordering rule between the statement rule and the expression one
        case NodeType::n_expr_const:
            return const_fold(static_cast<const ConstExprNode *>(expr)->operand);

        // **a cast folds exactly when it does not change the value.** the rule in one line, and it is
        // what keeps this from becoming a second answer to TypeLowering::coerce_value: the widening a
        // reconciliation performs leaves the value alone, so folding through it cannot disagree with what
        // codegen emits, while a *narrowing* is precisely a conversion that changes the value and is
        // refused rather than reproduced here.
        //
        // this arm is not an optional nicety. the parser reconciles `LIMIT > 50` by wrapping the int32
        // side in a `cast<usize>`, so without it the most ordinary `const if` over a constant - the
        // reason a constant can be compared at all - would be refused
        case NodeType::n_type_cast: {
            const auto &cast = *static_cast<const TypeCastNode *>(expr);
            const ConstFoldResult operand = const_fold(cast.expr);

            if (operand.result != Result::t_folded) {
                return operand;
            }

            // a bool is one bit and every other conversion into one is a question about truthiness the
            // language does not answer, so only bool-to-bool passes
            if (is_foldable_bool(cast.cast_to) && is_foldable_bool(operand.type)) {
                return operand;
            }

            if (!is_foldable_integer(cast.cast_to) || !is_foldable_integer(operand.type)) {
                return ConstFoldResult::refused(fmt::format(
                    "a conversion from '{}' to '{}' is not something the compiler folds - only a "
                    "widening between whole numbers leaves the value alone.",
                    operand.type.get_type_desciption(), cast.cast_to.get_type_desciption()));
            }

            if (!fits(cast.cast_to, operand.bits)) {
                return ConstFoldResult::refused(fmt::format(
                    "this value does not survive being read as a '{}', so folding it would give a "
                    "different answer from running it.", cast.cast_to.get_type_desciption()));
            }

            return ConstFoldResult::folded(cast.cast_to, operand.bits);
        }

        // everything else, by a **deny-list**: AST::storage_of's shape, and for its reason. a node kind
        // added later is not foldable, which is the conservative direction, and the refusal is located
        // so it gets looked at rather than silently mis-answered.
        //
        // an AST::ConstRefExprNode lands here and cannot: AST::ConstantExpander runs ahead of the
        // fixpoint, so one has already become a clone of its declaration's expression. unlike
        // BodyAnswerable's belt-and-braces silence about the same node, this one has a location to
        // report from
        default:
            return ConstFoldResult::refused(
                "this is not something the compiler can work out for itself - it needs storage, a call, "
                "or something that only exists while the program runs.");
    }
}
