#include "AST/ASTLiteralTyping.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCodeRef.h"
#include "AST/ASTCollector.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "AST/ASTModule.h"
#include "AST/ASTNode.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTOps.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/ReturnNode.h"
#include "AST/TypeCastNode.h"
#include "AST/VarDeclNode.h"
#include "External/infint.h"

#include <fmt/core.h>

namespace AST
{

    // ---------------------------------------------------------------------------
    // the spellings, and the defaults read off them
    // ---------------------------------------------------------------------------

    // trailing zeros off, at least one digit after the dot. the rendering an autocast writes into
    // override_literal_value, kept byte for byte from where this rule used to live in the parser
    static std::string trimmed_float_literal(std::string value)
    {
        value.erase(value.find_last_not_of('0') + 1, std::string::npos);

        if (value.back() == '.') {
            value += '0';
        }

        return value;
    }

    static std::string f64_literal(double value)
    {
        return trimmed_float_literal(std::to_string(value));
    }

    static std::string f32_literal(float value)
    {
        return trimmed_float_literal(std::to_string(value)) + "f";
    }

    ValueTypePrimitive LiteralIntExprNode::spelled_width(const TokenReference &token)
    {
        const InfInt value(token.value());

        return value > get_integer_size(ValueTypePrimitive::t_int32).get_max_positive_value()
            ? ValueTypePrimitive::t_int64
            : ValueTypePrimitive::t_int32;
    }

    // ---------------------------------------------------------------------------
    // is this literal's type a default nobody chose
    // ---------------------------------------------------------------------------

    // a number or bool literal, whoever decided its type
    static bool is_literal_primitive(const ExprNode *expr)
    {
        if (expr == nullptr) {
            return false;
        }

        switch (expr->get_node_type()) {
            case NodeType::n_literal_int:
            case NodeType::n_literal_float:
            case NodeType::n_literal_bool:
                return true;

            default:
                return false;
        }
    }

    bool is_untyped_literal(const ExprNode *expr)
    {
        return is_literal_primitive(expr)
            && !static_cast<const LiteralPrimitiveExprNode *>(expr)->type_was_chosen();
    }

    const TokenReference &literal_token_of(const ExprNode *expr)
    {
        assert(expr != nullptr && "asked for the token of a literal that is not there");

        switch (expr->get_node_type()) {
            case NodeType::n_literal_int:
            case NodeType::n_literal_float:
            case NodeType::n_literal_bool:
                return static_cast<const LiteralPrimitiveExprNode *>(expr)->token_literal;

            default:
                assert(false && "asked for the token of an expression that is not a literal");
                return static_cast<const LiteralPrimitiveExprNode *>(expr)->token_literal;
        }
    }

    // ---------------------------------------------------------------------------
    // what a literal becomes at a destination
    // ---------------------------------------------------------------------------

    static LiteralTyping unchanged(ExprNode *node)
    {
        LiteralTyping out;
        out.result = LiteralTyping::Result::t_unchanged;
        out.node = node;
        return out;
    }

    static LiteralTyping typed(ExprNode *node)
    {
        LiteralTyping out;
        out.result = LiteralTyping::Result::t_typed;
        out.node = node;
        return out;
    }

    static LiteralTyping refused(ExprNode *node, LiteralRefusal kind, std::string sentence)
    {
        LiteralTyping out;
        out.result = LiteralTyping::Result::t_refused;
        out.node = node;
        out.refusal_kind = kind;
        out.refusal = std::move(sentence);
        return out;
    }

    // **a bool is not a width, but 0 and 1 are the two values one holds.** Echo converts a *variable*
    // by comparing against zero - `bool $b = $n;` is `icmp ne 0` at runtime, exactly as `int32 $t = $f;`
    // truncates one - but a written literal is a compile-time question. `1` and `0` are what `echo`
    // prints for `true` and `false`; `3` is not either of those, and reading it as `true` would be
    // truthiness. the reverse is refused outright: there is no number `true` means
    static std::string bool_family_refusal(const ValueType &from, const ValueType &to)
    {
        return fmt::format(
            "a literal of type '{}' cannot be written where a '{}' is expected - Echo has no truthiness "
            "in a written literal, so say which of the two you meant",
            from.get_type_desciption(),
            to.get_type_desciption());
    }

    // does this decimal spelling sit inside `type`'s range. the two sentences are the ones the parser has
    // always produced, so the goldens that pin them do not move.
    //
    // `digits` is the value and `written` is what the author typed, and they are two arguments because a
    // radix spelling carries its decimal rendering in the override: an overflow reported as "the literal
    // '511'" about a line that says `0x1FF` names a number nobody wrote
    static std::optional<LiteralTyping> integer_range_refusal(
        ExprNode *node,
        const ValueType &type,
        const std::string &digits,
        const std::string &written
    )
    {
        const InfInt value(digits);
        const auto size = get_integer_size(type.get_primitive_type());

        if (value > size.get_max_positive_value()) {
            return refused(node, LiteralRefusal::t_overflow, fmt::format(
                "The literal '{}' is too large for the integer type '{}'. The maximum value is '{}'.",
                written,
                get_primitive_name(type.get_primitive_type()),
                size.get_max_positive_value()));
        }

        if (value < size.get_max_negative_value()) {
            return refused(node, LiteralRefusal::t_underflow, fmt::format(
                "The literal '{}' is too small for the integer type '{}'. The minimum value is '{}'.",
                written,
                get_primitive_name(type.get_primitive_type()),
                size.get_max_negative_value()));
        }

        return std::nullopt;
    }

    static LiteralTyping type_int_literal_at(
        LiteralIntExprNode &node,
        const ValueType &destination,
        NodeCollection &nodes
    )
    {
        const TokenReference token = node.token_literal;

        if (destination.is_floating_type()) {
            auto &casted = nodes.emplace_back<LiteralFloatExprNode>(token, destination.get_primitive_type());

            // seeded with the **effective** value, not the token: a hex or binary spelling carries its
            // decimal rendering in the override, and reading `0xFF` back as a float parses nothing
            casted.override_literal_value.emplace(node.effective_token_literal_value());

            // @TODO a very large integer loses precision as a float and nothing says so yet
            if (destination.get_primitive_type() == ValueTypePrimitive::t_float32) {
                casted.override_literal_value.emplace(f32_literal(casted.float_value()));
            }
            else {
                casted.override_literal_value.emplace(f64_literal(casted.double_value()));
            }

            return typed(&casted);
        }

        if (destination.is_integer_type()) {
            const InfInt value(node.effective_token_literal_value());

            if (destination.is_unsigned_integer() && value < 0) {
                return refused(&node, LiteralRefusal::t_invalid_conversion, fmt::format(
                    "The integer literal '{}' cannot be implicitly converted to an unsigned integer "
                    "because it is negative.",
                    node.effective_token_literal_value()));
            }

            if (auto out_of_range = integer_range_refusal(
                    &node, destination, node.effective_token_literal_value(), token.value())) {
                return *out_of_range;
            }

            auto &casted = nodes.emplace_back<LiteralIntExprNode>(token, destination.get_primitive_type());
            casted.override_literal_value = node.override_literal_value;

            return typed(&casted);
        }

        if (destination.is_boolean_type()) {
            const InfInt value(node.effective_token_literal_value());

            if (value == 0 || value == 1) {
                auto &casted = nodes.emplace_back<LiteralBoolExprNode>(token);
                casted.override_literal_value.emplace(value == 1 ? "true" : "false");
                casted.expected_primitive_type = ValueTypePrimitive::t_bool;

                return typed(&casted);
            }

            return refused(&node, LiteralRefusal::t_invalid_conversion,
                bool_family_refusal(node.result_type(), destination));
        }

        return refused(&node, LiteralRefusal::t_invalid_conversion, fmt::format(
            "The integer literal '{}' cannot be implicitly converted to the expected type '{}'.",
            node.effective_token_literal_value(),
            destination.get_type_desciption()));
    }

    static LiteralTyping type_float_literal_at(
        LiteralFloatExprNode &node,
        const ValueType &destination,
        NodeCollection &nodes
    )
    {
        const TokenReference token = node.token_literal;

        if (destination.is_floating_type()) {
            // the value survives either way - a narrowing only loses precision, which is a warning and
            // not a refusal, so the node is minted before the check rather than after it
            auto &casted = nodes.emplace_back<LiteralFloatExprNode>(token, destination.get_primitive_type());
            LiteralTyping out = typed(&casted);

            if (node.result_type().will_fit_into(destination) == false) {
                const double as_double = std::stod(node.get_fvalue_string());
                const float as_float = (float) as_double;

                // no warning where the round trip is exact - there is no point telling somebody that
                // 1.0 is still 1.0 in 32 bits
                if (as_double != (double) as_float) {
                    out.warning = fmt::format(
                        "The literal '{}' is stored in 32bit float which will result in the effctive value {}",
                        node.get_fvalue_string(),
                        as_float);

                    casted.override_literal_value.emplace(f32_literal(as_float));
                }
                else {
                    casted.override_literal_value.emplace(node.get_fvalue_string() + "f");
                }
            }

            // a float64 destination takes the value without the `f` suffix, if the spelling carried one
            if (destination.get_primitive_type() == ValueTypePrimitive::t_float64
                && node.effective_token_literal_value().back() == 'f') {
                casted.override_literal_value.emplace(node.get_fvalue_string());
            }

            return out;
        }

        if (destination.is_integer_type()) {
            const std::string spelling = node.get_fvalue_string();
            const double as_double = std::stod(spelling);

            // a non-zero decimal is an error rather than a warning: the author wrote a value this
            // destination cannot hold, and truncating it silently is the whole of B37
            if (as_double != (double) (long long) as_double) {
                return refused(&node, LiteralRefusal::t_invalid_conversion, fmt::format(
                    "The floating point number literal '{}' cannot be implicitly converted to an integer "
                    "type due to non zero decimal values.",
                    spelling));
            }

            const std::string digits = spelling.substr(0, spelling.find('.'));

            if (auto out_of_range = integer_range_refusal(&node, destination, digits, digits)) {
                return *out_of_range;
            }

            auto &casted = nodes.emplace_back<LiteralIntExprNode>(token, destination.get_primitive_type());
            casted.override_literal_value.emplace(digits);

            return typed(&casted);
        }

        if (destination.is_boolean_type()) {
            return refused(&node, LiteralRefusal::t_invalid_conversion,
                bool_family_refusal(node.result_type(), destination));
        }

        return refused(&node, LiteralRefusal::t_invalid_conversion, fmt::format(
            "The floating point number literal '{}' cannot be implicitly converted to the expected type '{}'.",
            node.get_fvalue_string(),
            destination.get_type_desciption()));
    }

    static LiteralTyping type_bool_literal_at(LiteralBoolExprNode &node, const ValueType &destination)
    {
        if (destination.is_boolean_type()) {
            return unchanged(&node);
        }

        // the mirror of the arm above, and refused for its reason: `int32 $k = true;` names no number
        return refused(&node, LiteralRefusal::t_invalid_conversion,
            bool_family_refusal(node.result_type(), destination));
    }

    LiteralTyping type_literal_at(ExprNode *literal, const ValueType &destination, NodeCollection &nodes)
    {
        if (literal == nullptr) {
            return unchanged(literal);
        }

        // **deliberately not AST::can_type_a_literal.** that one answers a parse-time question - may a
        // hint for this type be threaded down into an expression's operands before it is parsed - and a
        // `bool` is exactly the type for which the answer is no while the answer *here* is the 0/1
        // conversion. the two differ by that one case and by nothing else
        if (!destination.is_primitive() || destination.is_void()) {
            return unchanged(literal);
        }

        switch (literal->get_node_type()) {
            case NodeType::n_literal_int:
                return type_int_literal_at(static_cast<LiteralIntExprNode &>(*literal), destination, nodes);

            case NodeType::n_literal_float:
                return type_float_literal_at(static_cast<LiteralFloatExprNode &>(*literal), destination, nodes);

            case NodeType::n_literal_bool:
                return type_bool_literal_at(static_cast<LiteralBoolExprNode &>(*literal), destination);

            default:
                return unchanged(literal);
        }
    }

    // ---------------------------------------------------------------------------
    // reporting, for the four askers that all hold a collector
    // ---------------------------------------------------------------------------

    CodeRef code_ref_at_literal(const CodeRef &within, const ExprNode *literal)
    {
        return CodeRef { within.module, within.file, literal_token_of(literal).make_slice() };
    }

    void report_literal_refusal(
        Collector &collector, const CodeRef &at, LiteralRefusal kind, const std::string &sentence)
    {
        switch (kind) {
            case LiteralRefusal::t_overflow:
                collector.collect_issue<Issue::IntegerOverflow>(at, sentence);
                break;

            case LiteralRefusal::t_underflow:
                collector.collect_issue<Issue::IntegerUnderflow>(at, sentence);
                break;

            case LiteralRefusal::t_invalid_conversion:
                collector.collect_issue<Issue::InvalidTypeConversion>(at, sentence);
                break;
        }
    }

    void report_literal_refusal(Collector &collector, const CodeRef &at, const LiteralTyping &typing)
    {
        assert(typing.result == LiteralTyping::Result::t_refused
            && "reporting a literal that was not refused");

        report_literal_refusal(collector, at, typing.refusal_kind, typing.refusal);
    }

    void report_literal_warning(
        Collector &collector, const CodeRef &at, const std::optional<std::string> &warning)
    {
        if (!warning.has_value()) {
            return;
        }

        collector.collect_issue<Issue::LossOfPrecision>(at, *warning);
    }

    void report_literal_warning(Collector &collector, const CodeRef &at, const LiteralTyping &typing)
    {
        report_literal_warning(collector, at, typing.warning);
    }

    // ---------------------------------------------------------------------------
    // what two operands of one binary expression agree on
    // ---------------------------------------------------------------------------

    static bool same_numeric_family(const ValueType &a, const ValueType &b)
    {
        return (a.is_integer_type() && b.is_integer_type())
            || (a.is_floating_type() && b.is_floating_type());
    }

    // would reconciling `typed` into `common` reinterpret the value rather than widen it - the
    // same-width signedness flip that made `uint32 $u; $u / 2` read as a negative number
    static bool reinterprets(const ValueType &typed_side, const ValueType &common)
    {
        if (!typed_side.is_integer_type() || !common.is_integer_type()) {
            return false;
        }

        if (typed_side.get_primitive_type() == common.get_primitive_type()) {
            return false;
        }

        const size_t typed_size = get_primitive_size(typed_side.get_primitive_type());
        const size_t common_size = get_primitive_size(common.get_primitive_type());

        // a same-width signedness flip rereads the bits; a narrowing drops high ones.
        // widening is neither - `uint8 $u + 256` is int32 arithmetic and value-preserving
        if (common_size < typed_size) {
            return true;
        }

        return common_size == typed_size
            && typed_side.is_signed_integer() != common.is_signed_integer();
    }

    BinaryReconciliation reconcile_binary_operands(
        const Operator *op, ExprNode *lhs, ExprNode *rhs, NodeCollection &nodes)
    {
        BinaryReconciliation out;
        out.lhs = lhs;
        out.rhs = rhs;

        if (lhs == nullptr || rhs == nullptr) {
            return out;
        }

        // a shift's right operand is a *count* and not a second value, so there is nothing to reconcile
        if (!binary_reconciles_operands(op)) {
            return out;
        }

        const ValueType left = lhs->result_type();
        const ValueType right = rhs->result_type();

        const bool left_defers = is_untyped_literal(lhs);
        const bool right_defers = is_untyped_literal(rhs);

        // **the literal has no opinion.** exactly one side defers and both are the same kind of number,
        // so the typed side is the answer and the literal is retyped rather than the variable cast
        if (left_defers != right_defers && same_numeric_family(left, right)) {
            ExprNode *literal = left_defers ? lhs : rhs;
            const ValueType &decided = left_defers ? right : left;

            const LiteralTyping typing = type_literal_at(literal, decided, nodes);

            if (typing.result == LiteralTyping::Result::t_typed) {
                (left_defers ? out.lhs : out.rhs) = typing.node;
                out.result = BinaryReconciliation::Result::t_reconciled;
                out.warning = typing.warning;
                out.warned_operand = typing.node;

                return out;
            }

            // it did not fit. fall through to the ordinary reconciliation, which is right whenever it
            // only *widens* the typed side - `int32 $n + 3000000000` is int64 arithmetic and always was
            const auto common = common_numeric_type(left, right);

            if (common.has_value() && reinterprets(decided, *common)) {
                out.result = BinaryReconciliation::Result::t_refused;
                out.refusal_kind = LiteralRefusal::t_invalid_conversion;
                out.refused_operand = static_cast<LiteralPrimitiveExprNode *>(literal);
                out.refusal = fmt::format(
                    "the literal '{}' does not fit a '{}', and reconciling the two would read that "
                    "operand as a '{}' instead - write the conversion if that is what you meant",
                    out.refused_operand->effective_token_literal_value(),
                    decided.get_type_desciption(),
                    common->get_type_desciption());

                return out;
            }
        }

        const auto common = common_numeric_type(left, right);

        if (!common.has_value()) {
            return out;
        }

        // exactly one side differs: the common type is always one of the two
        const bool left_loses = left.get_primitive_type() != common->get_primitive_type();
        ExprNode *loser = left_loses ? lhs : rhs;

        // **a literal is still retyped rather than cast, even where it did not decide the answer.** the
        // step above is about which side the answer comes *from*; this is about how the other side gets
        // there, and a literal gets there by being written at the type instead of converted to it. that is
        // what keeps `2 * 3.14f` two float32 literals rather than a cast around an int32 one - and it is
        // the one thing the parser's try_implicit_cast did that AST::OperatorRewriter's copy never could
        if (is_literal_primitive(loser)) {
            const LiteralTyping typing = type_literal_at(loser, *common, nodes);

            if (typing.result == LiteralTyping::Result::t_refused) {
                out.result = BinaryReconciliation::Result::t_refused;
                out.refusal_kind = typing.refusal_kind;
                out.refused_operand = static_cast<LiteralPrimitiveExprNode *>(loser);
                out.refusal = typing.refusal;

                return out;
            }

            (left_loses ? out.lhs : out.rhs) = typing.node;
            out.result = BinaryReconciliation::Result::t_reconciled;
            out.warning = typing.warning;
            out.warned_operand = typing.node;

            return out;
        }

        (left_loses ? out.lhs : out.rhs) = &nodes.emplace_back<TypeCastNode>(*common, loser, true);

        out.result = BinaryReconciliation::Result::t_reconciled;

        return out;
    }

    void report_binary_reconciliation(
        Collector &collector,
        const Module *module,
        const File *file,
        const BinaryReconciliation &reconciled)
    {
        const auto at = [&](const ExprNode *literal) {
            return CodeRef { module, file, literal_token_of(literal).make_slice() };
        };

        if (reconciled.warning.has_value()) {
            report_literal_warning(collector, at(reconciled.warned_operand), reconciled.warning);
        }

        if (reconciled.result == BinaryReconciliation::Result::t_refused) {
            report_literal_refusal(
                collector,
                at(reconciled.refused_operand),
                reconciled.refusal_kind,
                reconciled.refusal);
        }
    }

    // ---------------------------------------------------------------------------
    // the post-parse moment, on the live tree
    // ---------------------------------------------------------------------------

    class DestinationLiteralTyping : public RecursiveVisitor
    {
    public:
        DestinationLiteralTyping(Module &module, Collector &collector)
            : _module(module), _collector(collector)
        {}

        File *file = nullptr;
        bool changed = false;

        void visitFunctionDecl(FunctionDeclNode &node) override
        {
            if (node.is_generic()) {
                return;
            }

            const ValueType enclosing = _return;
            _return = value_type_of(node.get_return_type());

            RecursiveVisitor::visitFunctionDecl(node);

            _return = enclosing;
        }

        void visitVarDecl(VarDeclNode &node) override
        {
            // a guard's binding is not this walk's: its declared type is deliberately one level
            // less nullable than what the initializer produces
            if (node.has_type() && node.init_expr != nullptr && !node.binds_unwrapped) {
                write_at(node.init_expr, value_type_of(node.type()), node.token_varname);
            }

            RecursiveVisitor::visitVarDecl(node);
        }

        void visit_assign(AssignNode &node) override
        {
            if (node.target != nullptr && node.value_expr != nullptr) {
                // the *storage's* type, peeled through the borrow an element operator hands back -
                // AST::value_result_type, the same peel Parser::parse_varexpr makes for a written
                // assignment. an index that has not resolved yet answers void and waits a round
                write_at(
                    node.value_expr,
                    value_result_type(*node.target),
                    node.token_assign);
            }

            RecursiveVisitor::visit_assign(node);
        }

        void visitReturn(ReturnNode &node) override
        {
            if (node.expr != nullptr && node.token_return.has_value()) {
                write_at(node.expr, _return, *node.token_return);
            }

            RecursiveVisitor::visitReturn(node);
        }

    private:
        void write_at(ExprNode *&slot, const ValueType &destination, const TokenReference &at)
        {
            if (!is_untyped_literal(slot) || is_undetermined_type(destination)) {
                return;
            }

            const LiteralTyping typing = type_literal_at(slot, destination, _module.nodes);

            if (typing.result == LiteralTyping::Result::t_unchanged) {
                return;
            }

            const CodeRef here = code_ref_at_literal(CodeRef{&_module, file, at.make_slice()}, slot);

            report_literal_warning(_collector, here, typing);

            if (typing.result == LiteralTyping::Result::t_refused) {
                report_literal_refusal(_collector, here, typing);
                return;
            }

            slot = typing.node;
            changed = true;
        }

        Module &_module;
        Collector &_collector;
        ValueType _return = ValueType::void_type();
    };

    bool type_destination_literals(Bundle &bundle)
    {
        bool progressed = false;

        for (auto &module_ptr : bundle.modules) {
            DestinationLiteralTyping walker(*module_ptr, bundle.collector);

            for (auto &file : module_ptr->files()) {
                walker.file = &file;

                if (file.root != nullptr) {
                    file.root->accept(walker);
                }
            }

            progressed |= walker.changed;
        }

        return progressed;
    }

};  // namespace AST
