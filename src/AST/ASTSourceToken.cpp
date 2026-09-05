#include "AST/ASTSourceToken.h"

#include "AST/AssignNode.h"
#include "AST/AttributeNode.h"
#include "AST/ConstDeclNode.h"
#include "AST/ConstExprNode.h"
#include "AST/ConstIfNode.h"
#include "AST/ConstRefExprNode.h"
#include "AST/GenericValueExprNode.h"
#include "AST/ExprNode.h"
#include "AST/ForeachNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/GuardNode.h"
#include "AST/IfStatementNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/LoopControlNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/NamespaceDeclNode.h"
#include "AST/UseDeclNode.h"
#include "AST/NamespaceNode.h"
#include "AST/NullNode.h"
#include "AST/OperatorNode.h"
#include "AST/ReleaseNode.h"
#include "AST/ReturnNode.h"
#include "AST/MatchExprNode.h"
#include "AST/ScopeNode.h"
#include "AST/StaticPropertyExprNode.h"
#include "AST/StringInterpolationNode.h"
#include "AST/TemporaryBindExprNode.h"
#include "AST/TypeCastNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/ForStatementNode.h"

#include <cassert>

namespace AST
{

namespace
{
    // an optional token that may legitimately be absent - a synthesized `return`, a `null` the
    // monomorphizer minted, a type node with no written name. spelled once because four arms want it
    const TokenReference *or_null(const std::optional<TokenReference> &token)
    {
        return token.has_value() ? &token.value() : nullptr;
    }

    // an owned edge that stands in for the node itself. `&$x` was never written by anybody, so it
    // means whatever its operand means - and a null operand is a shape error elsewhere, not here
    const TokenReference *through(const Node *operand)
    {
        return operand != nullptr ? source_token_of(*operand) : nullptr;
    }
};

const TokenReference *source_token_of(const Node &node)
{
    // **no `default:`.** The whole value of this function is that a node kind added later cannot
    // quietly inherit somebody else's answer - see the header. Every arm below is either a token the
    // author wrote, an edge that stands in for one, or an explicit null with a reason
    switch (node.get_node_type()) {

        // -- statements ---------------------------------------------------------------------------

        // the opening brace, when a parser wrote one down. null for the many scopes nothing wrote - a
        // function body's own, a lowered `foreach`'s, a `const if` arm spliced in - and null is the
        // right answer there rather than a borrowed position
        case NodeType::n_scope:
            return or_null(static_cast<const ScopeNode &>(node).token_brace);

        case NodeType::n_vardecl:
            return &static_cast<const VarDeclNode &>(node).token_varname;

        case NodeType::n_const_decl:
            return &static_cast<const ConstDeclNode &>(node).token_name;

        case NodeType::n_assign:
            return &static_cast<const AssignNode &>(node).token_assign;

        // a `return` with no token is one a pass wrote - a constructor's implicit `return $this`.
        // it falls through to the returned expression rather than answering null, which is where the
        // value it hands back was actually written
        case NodeType::n_func_return:
        {
            const auto &ret = static_cast<const ReturnNode &>(node);
            const TokenReference *written = or_null(ret.token_return);
            return written != nullptr ? written : through(ret.expr);
        }

        case NodeType::n_guard:
            return &static_cast<const GuardNode &>(node).token;

        case NodeType::n_loop_control:
            return &static_cast<const LoopControlNode &>(node).token;

        case NodeType::n_foreach:
            return &static_cast<const ForeachNode &>(node).token_foreach;

        case NodeType::n_const_if:
            return &static_cast<const ConstIfNode &>(node).token_const;

        // the keyword, falling back to the condition for the ones a pass minted: AST::ForeachLowering
        // builds a `while` out of nothing anybody wrote, and AST::ConstFolding splices arms in. The
        // fallback is not a guess - a condition sits on the keyword's line in every program a person
        // writes, and the two only come apart where there was no keyword to begin with
        case NodeType::n_if_statement:
        {
            const auto &branch = static_cast<const IfStatementNode &>(node);
            return branch.token_if.has_value() ? &branch.token_if.value() : through(branch.condition);
        }

        case NodeType::n_while_statement:
        {
            const auto &loop = static_cast<const WhileStatementNode &>(node);
            return loop.token_while.has_value() ? &loop.token_while.value() : through(loop.condition);
        }

        // no `has_value` dance beside the one above: nothing mints a `for` but the parser, so there is
        // always a keyword behind it
        case NodeType::n_for_statement:
            return &static_cast<const ForStatementNode &>(node).token_for;

        // synthesized by AST::OwnershipPass, always. It stands for the value going away, so it means
        // whatever named that value
        case NodeType::n_release:
            return through(static_cast<const ReleaseNode &>(node).target);

        // -- declarations -------------------------------------------------------------------------

        case NodeType::n_func_decl:
        {
            const auto &decl = static_cast<const FunctionDeclNode &>(node);
            return decl.name_token.has_value() || decl.declaration_token.has_value()
                ? &decl.declaration_site_token()
                : nullptr;
        }

        case NodeType::n_type_decl:
            return or_null(static_cast<const TypeDeclNode &>(node).name_token);

        case NodeType::n_type:
            return or_null(static_cast<const TypeNode &>(node).type_token);

        case NodeType::n_attribute:
            return &static_cast<const AttributeNode &>(node).attribute_id;

        case NodeType::n_operator:
            return &static_cast<const OperatorNode &>(node).token_literal;

        // a namespace declaration is a header line with no single token standing for it, and a
        // NamespaceNode is a marker a pass left behind. Neither is ever a position anybody wants
        case NodeType::n_namespace_decl:
        case NodeType::n_use_decl:
        case NodeType::n_namespace:
            return nullptr;

        // -- expressions --------------------------------------------------------------------------

        case NodeType::n_member_access:
            return &static_cast<const MemberAccessNode &>(node).get_member_name();

        case NodeType::n_expr_index:
            return &static_cast<const IndexExprNode &>(node).token_bracket;

        case NodeType::n_expr_peel:
            return &static_cast<const PointerValueNode &>(node).token_peel;

        case NodeType::n_expr_move:
            return &static_cast<const MoveExprNode &>(node).token_move;

        case NodeType::n_expr_call:
            return &static_cast<const FunctionCallExprNode &>(node).token_function_name;

        // the member name that asked for the temporary, which is where every other diagnostic about
        // one points too
        case NodeType::n_expr_temp_bind:
            return &static_cast<const TemporaryBindExprNode &>(node).token;

        // the `match` keyword. every diagnostic about the form as a whole - non-exhaustive, arms that
        // do not meet at a type - points here, where an arm's own diagnostics point at that arm
        case NodeType::n_expr_match:
            return &static_cast<const MatchExprNode &>(node).token;

        // the shapes nobody wrote, each standing in for its operand: `&`, a deref, an implicit cast,
        // the retain a class assignment owes
        case NodeType::n_expr_addrof:
            return through(static_cast<const AddrOfExprNode &>(node).operand);

        case NodeType::n_expr_deref:
            return through(static_cast<const DerefExprNode &>(node).operand);

        case NodeType::n_type_cast:
        {
            // a written cast points at the `as` (or the `T` of `T(...)`). an implicit one has no
            // token of its own and stands in for its operand, the way a deref does
            const auto &cast = static_cast<const TypeCastNode &>(node);

            if (!cast.is_implcit && cast.token.has_value() && cast.token->is_valid()) {
                return &*cast.token;
            }

            return through(cast.expr);
        }

        case NodeType::n_expr_retain:
            return through(static_cast<const RetainExprNode &>(node).operand);

        // the three numeric literals share LiteralPrimitiveExprNode, so one arm answers all of them;
        // a string literal is its own class with the same field name
        case NodeType::n_literal:
        case NodeType::n_literal_float:
        case NodeType::n_literal_int:
        case NodeType::n_literal_bool:
            return &static_cast<const LiteralPrimitiveExprNode &>(node).token_literal;

        case NodeType::n_literal_string:
            return &static_cast<const LiteralStringExprNode &>(node).token_literal;

        // the opening chunk, so a diagnostic underlines the literal from its quote rather than one of
        // the holes inside it - a hole that wants to be located carries its own token
        case NodeType::n_string_interpolation:
            return &static_cast<const StringInterpolationExprNode &>(node).token_string;

        case NodeType::n_expr_static_property:
            return &static_cast<const StaticPropertyExprNode &>(node).token_name;

        // an operator carries the token, not the expression - so `$a + $b` locates at the `+`, which
        // is the one character that names the whole result
        case NodeType::n_expr_binary:
            return &static_cast<const BinaryExprNode &>(node).op_node->token_literal;

        case NodeType::n_expr_unary:
            return &static_cast<const UnaryExprNode &>(node).token_operator;

        case NodeType::n_varref:
        {
            const auto &ref = static_cast<const VarRefNode &>(node);
            return ref.is_var() ? &ref.get_var().use_token() : nullptr;
        }

        case NodeType::n_var:
            return &static_cast<const VarNode &>(node).use_token();

        // the three nullability forms and the marker standing for a chain's unwrapped base
        case NodeType::n_expr_optional_chain:
            return &static_cast<const OptionalChainExprNode &>(node).token;

        case NodeType::n_expr_null_coalesce:
            return &static_cast<const NullCoalesceExprNode &>(node).token;

        case NodeType::n_expr_strong:
            return &static_cast<const StrongExprNode &>(node).token;

        case NodeType::n_expr_chain_base:
            return &static_cast<const ChainBaseNode &>(node).token;

        case NodeType::n_null:
            return or_null(static_cast<const NullNode &>(node).token_null);

        case NodeType::n_expr_array_literal:
            return &static_cast<const ArrayLiteralExprNode &>(node).token_bracket;

        case NodeType::n_expr_class_alloc:
            return &static_cast<const ClassAllocExprNode &>(node).token_type;

        case NodeType::n_expr_closure:
            return &static_cast<const ClosureExprNode &>(node).token;

        case NodeType::n_expr_indirect_call:
            return &static_cast<const IndirectCallExprNode &>(node).token;

        case NodeType::n_expr_function_ref:
            return &static_cast<const FunctionRefExprNode &>(node).token_amp;

        case NodeType::n_expr_instanceof:
            return &static_cast<const InstanceOfExprNode &>(node).token_instanceof;

        case NodeType::n_expr_const_ref:
            return &static_cast<const ConstRefExprNode &>(node).token_name;

        case NodeType::n_expr_generic_value:
            return &static_cast<const GenericValueExprNode &>(node).token_name;

        case NodeType::n_expr_const:
            return &static_cast<const ConstExprNode &>(node).token_const;

        // the empty expression a parser hands back where a value was optional. It stands for nothing
        // the author wrote, which is exactly what it means
        case NodeType::n_expr_void:
            return nullptr;

        // reserved in the enum with no node class behind it - a node that cannot exist cannot be located
        case NodeType::n_void:
            return nullptr;
    }

    // unreachable: the switch above is total over NodeType and has no default, so a kind added
    // without an arm is a compile error rather than a fall-through to here
    return nullptr;
}

const TokenReference &location_of_expression(ExprNode *expr)
{
    const TokenReference *token = source_token_of(*expr);

    assert(token != nullptr && "no token to locate this expression at");

    return *token;
}

};  // namespace AST
