#include "AST/ASTOperatorSemantics.h"

#include "AST/ASTPlaceExpr.h"
#include "AST/ExprNode.h"

#include <fmt/core.h>

namespace AST
{
    std::string operator_function_name(const std::string &spelling, OpFixity fixity)
    {
        switch (fixity) {
            case OpFixity::t_infix:
                return "operator " + spelling;
            case OpFixity::t_prefix:
                return "operator prefix " + spelling;
            case OpFixity::t_suffix:
                return "operator suffix " + spelling;
        }

        return "operator " + spelling;
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

    namespace
    {
        // `null` has no type of its own, so which operand was written as one is a question about the
        // *node*. the two operand builders share it rather than each spelling the tag
        bool written_as_null(const ExprNode *expr)
        {
            return expr != nullptr && expr->get_node_type() == NodeType::n_null;
        }
    }

    OperandFacts parse_time_operand(const ExprNode *expr)
    {
        if (expr == nullptr) {
            return {};
        }

        return OperandFacts{value_result_type(*expr), written_as_null(expr)};
    }

    OperandFacts adjusted_operand(const ExprNode *expr)
    {
        if (expr == nullptr) {
            return {};
        }

        return OperandFacts{expr->result_type(), written_as_null(expr)};
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
};
