#include "AST/ASTOperatorSemantics.h"

#include "AST/ASTCollector.h"
#include "AST/ASTModule.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTNullability.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ExprNode.h"

#include <fmt/core.h>

namespace AST
{
    std::string operator_function_name(const std::string &spelling, OpFixity fixity)
    {
        // **only the unary fixities carry a word**, and only they need one: a prefix and a suffix
        // declaration of one symbol take one parameter of the same type, so they are otherwise the
        // same signature and would clash as a DuplicateFunctionSignature
        //
        // infix needs none - it separates from both on arity, which AST::match_function compares
        // before it looks at any type - and neither does the index form, whose spelling can only ever
        // be an index operator, since `[` and `]` are refused inside every other symbol
        switch (fixity) {
            case OpFixity::t_prefix:
                return "operator prefix " + spelling;
            case OpFixity::t_suffix:
                return "operator suffix " + spelling;
            case OpFixity::t_infix:
            case OpFixity::t_index:
                break;
        }

        return "operator " + spelling;
    }

    FunctionCallExprNode &build_operator_call_node(
        Module &module,
        Collector &collector,
        const std::string &spelling,
        OpFixity fixity,
        const TokenReference &at,
        std::vector<ExprNode *> operands)
    {
        // the overload set's key, derived from the symbol and the position it was consumed in - the
        // caller names that position once, in the gate that decided to come here
        const std::string decorated_name = operator_function_name(spelling, fixity);

        // the name is **virtual**, because no token in the source spells it. typed t_identifier so
        // AST::is_print_call - which keys on t_echo - cannot mistake it for `echo`
        const TokenReference name_token =
            module.make_virtual_token(decorated_name, Token::Type::t_identifier, at);

        auto &call = module.nodes.emplace_back<FunctionCallExprNode>(name_token, std::move(operands));

        // the root namespace, for the reason the header gives
        call.lookup_namespace = &collector.namespaces.root();

        return call;
    }

    std::string mangle_operator_name(const std::string &decorated_name)
    {
        std::string mangled;
        mangled.reserve(decorated_name.size());

        for (const unsigned char c : decorated_name) {
            const bool safe = (c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9')
                || c == '_';

            if (safe) {
                mangled.push_back(static_cast<char>(c));
            } else {
                mangled += fmt::format("x{:02x}", c);
            }
        }

        return mangled;
    }

    OperandFacts parse_time_operand(const ExprNode *expr, const ValueType &result_type)
    {
        if (expr == nullptr) {
            return {};
        }

        return OperandFacts{value_result_type(*expr, result_type), is_written_null(expr)};
    }

    OperandFacts parse_time_operand(const ExprNode *expr)
    {
        if (expr == nullptr) {
            return {};
        }

        return parse_time_operand(expr, expr->result_type());
    }

    OperandFacts adjusted_operand(const ExprNode *expr)
    {
        if (expr == nullptr) {
            return {};
        }

        return OperandFacts{expr->result_type(), is_written_null(expr)};
    }

    bool binary_has_builtin_meaning(
        const Operator *op, const OperandFacts &lhs, const OperandFacts &rhs)
    {
        if (op == nullptr) {
            return false;
        }

        // the language spells no meaning for a declared symbol, so there is nothing to fall through
        // to and nothing to weigh a declaration against
        if (op->is_custom()) {
            return false;
        }

        // `==` and `!=` over two class handles is an address comparison, which codegen does lower -
        // it is how two references are told apart and how a null one is detected. a null operand
        // types as void rather than as a class, so it is admitted the same way
        const bool class_identity = op->is_identity_comparison()
            && (lhs.type.is_class() || rhs.type.is_class())
            && (lhs.type.is_class() || lhs.is_null)
            && (rhs.type.is_class() || rhs.is_null);

        if (class_identity) {
            return true;
        }

        // **anything nullable against `null`**, which is the same question one level up: not "are these
        // two the same object" but "is this one there at all". the language lowers it for every shape - an
        // address comparison where the type has a null value of its own, a tag test where it does not -
        // so it has a built-in meaning even over a struct, where `==` otherwise has none
        //
        // one side has to be a written `null`. `$a == $b` over two `Point?`s is a question about the
        // *values*, which is exactly what a declared `==` on Point would be for, so it is left to fall
        // through to the declaration the way it always did
        const bool presence_test = op->is_identity_comparison()
            && ((lhs.is_null && destination_admits_null(rhs.type))
                || (rhs.is_null && destination_admits_null(lhs.type)));

        if (presence_test) {
            return true;
        }

        // everything else on a struct or a class operand has no lowering at all. undeterminable
        // operands - unknown, void, a bare type parameter - are left alone: they say nothing either
        // way, and a program that has one has already been told why
        return !lhs.type.has_complex_type() && !rhs.type.has_complex_type();
    }

    bool unary_has_builtin_meaning(const Operator *op, const OperandFacts &operand)
    {
        if (op == nullptr || op->is_custom()) {
            return false;
        }

        // negation over a number, which is the whole of gen_unary_expr - plus unary `+`, which the
        // parser folds away because it carries no semantics. that fold *is* a built-in meaning, and
        // saying so here is what keeps `+5` an int rather than a lookup for a declared `+` somebody
        // wrote for a struct
        //
        // an undeterminable operand says nothing either way, so it is admitted for the reason the
        // binary rule admits one
        if (op->type == Token::Type::t_op_sub || op->type == Token::Type::t_op_add) {
            return operand.type.is_integer_type()
                || operand.type.is_floating_type()
                || is_undetermined_type(operand.type)
                || operand.type.is_type_param();
        }

        return false;
    }

    std::optional<ValueType> common_numeric_type(const ValueType &lhs, const ValueType &rhs)
    {
        const bool lhs_numeric = lhs.is_integer_type() || lhs.is_floating_type();
        const bool rhs_numeric = rhs.is_integer_type() || rhs.is_floating_type();

        // both predicates already gate on is_primitive(), so get_primitive_type() below is answerable
        if (!lhs_numeric || !rhs_numeric || lhs.get_primitive_type() == rhs.get_primitive_type()) {
            return std::nullopt;
        }

        if (lhs.is_integer_type() && rhs.is_floating_type()) {
            return rhs;
        }

        if (lhs.is_floating_type() && rhs.is_integer_type()) {
            return lhs;
        }

        return get_primitive_size(lhs.get_primitive_type()) > get_primitive_size(rhs.get_primitive_type())
            ? lhs
            : rhs;
    }
};
