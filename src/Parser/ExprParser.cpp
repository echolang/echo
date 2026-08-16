#include "Parser/ExprParser.h"
#include "Parser/MatchParser.h"

#include "AST/ConstRefExprNode.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTOps.h"
#include "AST/ASTNullability.h"
#include "AST/ASTLiteralTyping.h"
#include "AST/ASTCFunction.h"
#include "AST/ASTConstantExpander.h"
#include "AST/ExprNode.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/VarRefNode.h"
#include "AST/VarNode.h"
#include "AST/OperatorNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/ASTStringLiteral.h"
#include "AST/StringInterpolationNode.h"
#include "AST/TypeCastNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/ASTMemberLookup.h"
#include "AST/NullNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/ASTConstness.h"

#include "External/infint.h"

#include "Parser/FuncCallParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/NamespaceParser.h"
#include "Parser/TypeParser.h"
#include "AST/TypeNode.h"
#include "AST/ConstExprNode.h"

#include <fmt/core.h>
#include <stack>

bool Parser::starts_const_expr(const Parser::Cursor &cursor)
{
    return cursor.is_type_sequence(0, { Token::Type::t_const, Token::Type::t_open_paren });
}

// **report what AST::type_literal_at decided**, and hand back whatever the literal became.
//
// the rule itself is AST::ASTLiteralTyping's. a helper living here would leave the five
// positions that are not this parser to grow their own answer or none at all. what stays
// behind is the half that has a CodeRef, the same split AST::ArrayLiteralLookup and
// AST::interface_erasure_refusal both make
//
// this is the rule. apply_literal_typing_if_wanted is the operand-hint gate
// (AST::can_type_a_literal); parse_expr_ref calls this directly so a bool destination
// still types 0/1 and refuses everything else
AST::NodeReference apply_literal_typing(
    Parser::Payload &payload, AST::ExprNode *literal, const AST::ValueType &destination)
{
    const AST::LiteralTyping typing =
        AST::type_literal_at(literal, destination, payload.context.module.nodes);

    const AST::CodeRef at = payload.context.code_ref(AST::literal_token_of(literal));

    AST::report_literal_warning(payload.collector, at, typing);

    if (typing.result == AST::LiteralTyping::Result::t_refused) {
        AST::report_literal_refusal(payload.collector, at, typing);

        return AST::make_void_ref();
    }

    // the node type is on the node - the literal may have become a *different* kind of literal,
    // and the shape of what came back is exactly what it decided
    return AST::NodeReference(typing.node->get_node_type(), typing.node);
}

// the same, for a destination that may be anything at all - the shape every literal parse arm wants
AST::NodeReference apply_literal_typing_if_wanted(
    Parser::Payload &payload, AST::ExprNode *literal, AST::TypeNode *expected_type)
{
    if (expected_type == nullptr || !AST::can_type_a_literal(expected_type->type)) {
        return AST::NodeReference(literal->get_node_type(), literal);
    }

    return apply_literal_typing(payload, literal, expected_type->type);
}

// an interpolated string literal, cursor on its `t_string_interp_begin`. answers null having reported
// when the token run does not close, which cannot happen for a run this lexer produced but can for one
// a conditional filter cut in half.
//
// the shape it reads is exactly what LexerFunction::StringLiteral emits:
//
//     begin("a")  <hole tokens>  [spec(">4")]  middle("b")  <hole tokens>  end("c")
//
// so the loop is "chunk, then a hole, then the chunk that closed it", and the `end` chunk is the one
// that stops it. that keeps `chunks.size() == holes.size() + 1` true by construction rather than by
// a check afterwards
AST::StringInterpolationExprNode *parse_string_interpolation(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    const auto begin_token = cursor.current();
    auto &node = payload.context.emplace_node<AST::StringInterpolationExprNode>(begin_token);

    // the type the literal *is*, stamped for LiteralStringExprNode's reason. with no stdlib there is
    // nothing to stamp and AST::InterpolationLowering is what says so, in a sentence about the
    // standard library rather than about this token
    if (payload.collector.core_types.has(AST::CoreTypeKind::t_string)) {
        node.core_string_type = payload.collector.core_types.string_type();
    }

    // decoded through AST::decode_string_chunk rather than decode_string_literal: the lexer already
    // cut the quotes off, and there is no second escape vocabulary
    auto take_chunk = [&payload, &node](const TokenReference &token) {
        std::string bytes;

        if (auto error = AST::decode_string_chunk(token.value(), bytes)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(token), error->message);
        }

        node.chunks.push_back(std::move(bytes));
    };

    take_chunk(begin_token);
    cursor.skip();

    while (true) {
        AST::StringInterpolationExprNode::Hole hole { nullptr, std::nullopt, cursor.current() };

        hole.expr = Parser::parse_expr(payload, nullptr);

        if (hole.expr == nullptr) {
            return nullptr;
        }

        if (cursor.is_type(Token::Type::t_string_interp_spec)) {
            hole.spec = cursor.current().value();
            cursor.skip();
        }

        node.holes.push_back(hole);

        if (cursor.is_type(Token::Type::t_string_interp_middle)) {
            take_chunk(cursor.current());
            cursor.skip();
            continue;
        }

        if (cursor.is_type(Token::Type::t_string_interp_end)) {
            take_chunk(cursor.current());
            cursor.skip();
            break;
        }

        payload.collect_unexpected_token(Token::Type::t_string_interp_end);
        return nullptr;
    }

    return &node;
}

// the comma separated expressions between a `[` and its `]`, cursor already past the opening bracket
// and left after the closing one. answers false having reported when the list does not close
//
// **one grammar for what may sit inside brackets**, because two positions ask: an index list
// (`$m[$r, $c]`) and an array literal (`[1, 2, 3]`). an empty list is legal at both - `$a[]` is the
// append slot and `[]` is an empty literal - so emptiness is the caller's question, not this one's
bool parse_bracketed_expr_list(Parser::Payload &payload, std::vector<AST::ExprNode *> &elements)
{
    auto &cursor = payload.cursor;

    while (!cursor.is_type(Token::Type::t_close_bracket)) {
        auto *element = Parser::parse_expr(payload, nullptr);

        if (element == nullptr) {
            return false;
        }

        elements.push_back(element);

        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
            continue;
        }

        break;
    }

    if (!cursor.is_type(Token::Type::t_close_bracket)) {
        payload.collect_unexpected_token(Token::Type::t_close_bracket);
        return false;
    }

    cursor.skip(); // `]`

    return true;
}

/**
 * FLOAT LITERAL
 * ----------------------------------------------------------------------------
 *
 * Parse a float literal and return a node reference to it.
 */
const AST::NodeReference parse_literal_float(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;

    auto current_token = cursor.current();
    auto &node = payload.context.emplace_node<AST::LiteralFloatExprNode>(current_token);
    cursor.skip();

    return apply_literal_typing_if_wanted(payload, &node, expected_type);
}

/**
 * INTEGER LITERAL
 * ----------------------------------------------------------------------------
 *
 * Parse a integer literal and return a node reference to it.
 */
const AST::NodeReference parse_literal_int(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;
    auto current_token = cursor.current();

    // which width a bare integer defaults to is AST::LiteralIntExprNode's own question now - it used
    // to be decided here and *stamped* as though a destination had chosen it, which is precisely what
    // left every later rule unable to tell a default apart from an answer
    auto &node = payload.context.emplace_node<AST::LiteralIntExprNode>(current_token);
    cursor.skip();

    return apply_literal_typing_if_wanted(payload, &node, expected_type);
}

// the decimal rendering of a `0x` or `0b` spelling, and the width its digit count defaults to.
// **one function for the two**, because the only thing that differs is the base and the prefix - and
// they had drifted already: the hex arm ignored its destination entirely, so `int32 $x = 0xFF;` was a
// uint8 node and `uint8 $y = 0x1FF;` silently stored 255
const AST::NodeReference parse_literal_radix(
    Parser::Payload &payload,
    AST::TypeNode *expected_type,
    int base,
    size_t digits_per_byte
)
{
    auto &cursor = payload.cursor;
    auto current_token = cursor.current();

    const std::string spelling = current_token.value();
    const size_t digits = spelling.length() - 2;  // the `0x` / `0b` prefix

    // the width the *spelling* asks for, which is a default and not a choice: a destination still
    // decides, and only where there is none does this stand
    AST::ValueTypePrimitive width = AST::ValueTypePrimitive::t_uint64;

    if (digits <= digits_per_byte) {
        width = AST::ValueTypePrimitive::t_uint8;
    } else if (digits <= digits_per_byte * 2) {
        width = AST::ValueTypePrimitive::t_uint16;
    } else if (digits <= digits_per_byte * 4) {
        width = AST::ValueTypePrimitive::t_uint32;
    }

    auto &node = payload.context.emplace_node<AST::LiteralIntExprNode>(
        current_token, AST::LiteralIntExprNode::DefaultWidth { width });

    // everything downstream reads a literal's value as decimal, so the radix spelling is carried in
    // the override the way an autocast's rewritten value is
    try {
        node.override_literal_value.emplace(
            std::to_string(std::stoull(spelling.substr(2), nullptr, base)));
    }
    catch (const std::exception &) {
        payload.collector.collect_issue<AST::Issue::IntegerOverflow>(
            payload.context.code_ref(current_token),
            fmt::format(
                "The literal '{}' does not fit any integer type - the largest is 'uint64'.",
                spelling));

        cursor.skip();
        return AST::make_void_ref();
    }

    cursor.skip();

    return apply_literal_typing_if_wanted(payload, &node, expected_type);
}

/**
 * HEX LITERAL
 * ----------------------------------------------------------------------------
 *
 * Parse a hex literal and return a node reference to it.
 */
const AST::NodeReference parse_literal_hex(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    return parse_literal_radix(payload, expected_type, 16, 2);
}

/**
 * BINARY LITERAL
 * ----------------------------------------------------------------------------
 *
 * Parse a binary literal and return a node reference to it.
 */
const AST::NodeReference parse_literal_binary(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    return parse_literal_radix(payload, expected_type, 2, 8);
}

/**
 * BOOLEAN LITERAL
 * ----------------------------------------------------------------------------
 *
 * Parse a boolean literal and return a node reference to it.
 */
const AST::NodeReference parse_literal_boolean(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;
    auto current_token = cursor.current();

    if (current_token.type() != Token::Type::t_bool_literal) {
        payload.collect_unexpected_token(Token::Type::t_bool_literal);
        return AST::make_void_ref();
    }

    auto &node = payload.context.emplace_node<AST::LiteralBoolExprNode>(current_token);
    cursor.skip();

    return apply_literal_typing_if_wanted(payload, &node, expected_type);
}

/**
 * BINARY EXPRESSION
 * ----------------------------------------------------------------------------
 *
 * Parse a binary expression, handles special cases like implicit casting
 * and returns a node reference to the resulting expression node.
 */
const AST::NodeReference parse_binary_expr(Parser::Payload &payload, AST::OperatorNode *op_node, AST::NodeReference lhs, AST::NodeReference rhs)
{
    // they have to be expr nodes
    auto lhs_expr = lhs.unsafe_ptr<AST::ExprNode>();
    auto rhs_expr = rhs.unsafe_ptr<AST::ExprNode>();

    if (lhs_expr == nullptr || rhs_expr == nullptr) {
        return AST::make_void_ref();
    }

    // read once, and read here: both the declared-operator gate below and every built-in arm under it
    // want them, and result_type() walks the operand's subtree - a member access resolves its property
    // by name - so asking twice is asking a question that cannot answer differently
    auto lhs_type = lhs_expr->result_type();
    auto rhs_type = rhs_expr->result_type();

    // **`??` is not a binary operator**, though it is spelled and parsed as one. it short-circuits, so it
    // cannot be a function of two already-evaluated operands the way every other infix symbol here is -
    // the right side must not run when the left is there. it gets its own node and its own lowering
    //
    // ahead of the declared-operator gate below because `??` is not declarable: it is spelled by the
    // language, and letting an overload set claim the symbol would make the short circuit optional
    if (op_node != nullptr && op_node->op != nullptr
        && op_node->op->type == Token::Type::t_qmark_qmark) {

        // the weak upgrade, through the one function all three forms share. after this the left side is an
        // ordinary nullable and everything below knows nothing about weak references
        AST::ExprNode *left = AST::optional_operand_of(
            lhs_expr, payload.context.module, op_node->token_literal);

        const AST::ValueType left_type = left->result_type();

        // a left side that can never be absent makes the right side dead code. an undetermined type
        // waits for a later round as ever, and AST::TypeChecker is the one that asks again
        const std::string refusal =
            AST::certainly_present_refusal(AST::OptionalForm::t_null_coalesce, left_type);

        if (!refusal.empty()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(op_node->token_literal), refusal);
            return AST::make_void_ref();
        }

        auto &node = payload.context.emplace_node<AST::NullCoalesceExprNode>(
            left, rhs_expr, op_node->token_literal);

        return AST::make_ref(node);
    }

    // **a declared operator, before the built-in arms below.** an operator is a function, so all this
    // does is hand the operands to the same overload resolution every call goes through - which is
    // why nothing downstream of here has an operator case at all
    //
    // the gate is asked of the *operator table*, filled by the type-name pass, rather than of the
    // overload set, filled by the declaration pass: this function runs during the declaration pass
    // too, for a struct property's `= ...` initializer, and asking a half-filled overload set there
    // would answer differently depending on which file was walked first
    if (op_node != nullptr && op_node->op != nullptr
        && op_node->op->has_fixity(AST::OpFixity::t_infix)
        && !AST::binary_has_builtin_meaning(
            op_node->op,
            AST::parse_time_operand(lhs_expr, lhs_type),
            AST::parse_time_operand(rhs_expr, rhs_type))) {

        auto *call = Parser::build_operator_call(
            payload, *op_node->op, AST::OpFixity::t_infix, op_node->token_literal,
            {lhs_expr, rhs_expr});

        if (call == nullptr) {
            return AST::make_void_ref();
        }

        return AST::make_ref(*call);
    }

    // **the numeric reconciliation, and it is AST::reconcile_binary_operands' whole answer** - the same
    // one AST::OperatorRewriter asks for the operands only a later pass gives a type to. a second
    // copy here would have no notion of which side *knows* what it is, so a literal's default
    // would cast the variable beside it down to meet it
    const AST::Operator *reconcile_op = op_node != nullptr ? op_node->op : nullptr;

    const AST::BinaryReconciliation reconciled = AST::reconcile_binary_operands(
        reconcile_op, lhs_expr, rhs_expr, payload.context.module.nodes);

    AST::report_binary_reconciliation(
        payload.collector, &payload.context.module, reconciled);

    if (reconciled.result == AST::BinaryReconciliation::Result::t_refused) {
        return AST::make_void_ref();
    }

    auto &node = payload.context.emplace_node<AST::BinaryExprNode>(
        op_node, reconciled.lhs, reconciled.rhs);

    return AST::make_ref(node);
}

AST::ExprNode *Parser::parse_expr(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto ref = parse_expr_ref(payload, expected_type);

    // probably a bad idea, but it should never be not a expr node
    return ref.unsafe_ptr<AST::ExprNode>();
}

// a declared operator matching at the cursor, or {} - the sequence lookup the yard, the prefix
// position and the suffix chain all go through, so "is there an operator here" is asked one way
//
// bounded by the cursor's range, because a module's token collection holds every one of its files
// back to back and a multi-token symbol at the end of one would otherwise match into the next
AST::OperatorRegistry::Match operator_match_at(Parser::Payload &payload, Parser::Cursor &cursor)
{
    if (cursor.is_done()) {
        return {};
    }

    return payload.collector.operators.match_at(cursor.current(), cursor.remaining());
}

// the **declared** operator with this fixity at the cursor, or {}. the fixity is part of the question
// because the three positions are different: a suffix-only `mm` must not be tried as an infix
// operator, and a bare identifier must not become an operator just because some symbol somewhere is
// spelled that way
//
// answers with the match rather than a bool because a caller that goes on to consume the symbol needs
// its token_count, and asking twice means matching twice - a sequence scan per declared symbol, at a
// position the parser is already standing on
AST::OperatorRegistry::Match declared_operator_at(
    Parser::Payload &payload, Parser::Cursor &cursor, AST::OpFixity fixity)
{
    const auto match = operator_match_at(payload, cursor);

    // has_fixity implies is_declared - the fixities *are* the declarations, so asking both reads as
    // two facts where there is one
    if (!match.has() || !match.op->has_fixity(fixity)) {
        return {};
    }

    return match;
}

// the same question where only the answer's existence is wanted, asked of **either expression
// position in one match**: is_expr_token wants a symbol that could *begin* an expression or *continue*
// one, and those are two fixities over the one symbol - so asking per fixity at an unmoved cursor runs
// the sequence scan twice for a single answer, at the token that ends every expression in the program
bool starts_declared_operator_in_expression(Parser::Payload &payload, Parser::Cursor &cursor)
{
    const auto match = operator_match_at(payload, cursor);

    return match.has()
        && (match.op->has_fixity(AST::OpFixity::t_prefix) || match.op->has_fixity(AST::OpFixity::t_infix));
}

bool Parser::starts_shorthand_call(Parser::Cursor &cursor)
{
    // **a `.` glued to an identifier, and that is the whole test** - the argument list is no longer
    // part of it, because `.cannot_connect` is a case of an enum the destination names and has no
    // arguments to write.
    //
    // the declared `..` is what the old shape guard was protecting, and it is still untouched: a range
    // is two adjacent `t_dot` tokens, so the token after the first is a `.` and never an identifier.
    // nor can a range reach here at all - it is infix, so it is read where an *operator* is expected
    // and this runs where an operand is
    return cursor.is_type(Token::Type::t_dot)
        && cursor.peek_is_type(1, Token::Type::t_identifier);
}

bool Parser::shorthand_call_has_arguments(Parser::Cursor &cursor)
{
    return cursor.peek_is_type(2, Token::Type::t_open_paren)
        || cursor.peek_is_type(2, Token::Type::t_open_angle);
}

bool is_expr_token(Parser::Payload &payload, Parser::Cursor &cursor)
{
    if (cursor.is_done()) {
        return false;
    }

    return cursor.is_type(Token::Type::t_floating_literal) ||
           cursor.is_type(Token::Type::t_integer_literal) ||
           cursor.is_type(Token::Type::t_hex_literal) ||
           cursor.is_type(Token::Type::t_binary_literal) ||
           cursor.is_type(Token::Type::t_bool_literal) ||
           cursor.is_type(Token::Type::t_varname) ||
           cursor.is_type(Token::Type::t_open_paren) ||
           cursor.is_type(Token::Type::t_close_paren) ||
           cursor.is_type(Token::Type::t_identifier) ||
           cursor.is_type(Token::Type::t_namespace_sep) ||
           cursor.is_type(Token::Type::t_string_literal) ||
           // **only the opening chunk.** the middle, end and spec tokens are what *close* a hole, and
           // an expression must stop dead at one - a hole holding `$a` has to end there rather than
           // reading the chunk after it as a second operand
           cursor.is_type(Token::Type::t_string_interp_begin) ||
           cursor.is_type(Token::Type::t_ref) ||
           cursor.is_type(Token::Type::t_ptr_of) ||
           cursor.is_type(Token::Type::t_null) ||
           // `ptr<T>(...)` is a cast, so a type keyword can begin an expression
           cursor.is_type(Token::Type::t_ptr) ||
           // and `weak($obj)` / `strong($w)` for the same reason. without these the shunting-yard loop
           // never enters, the expression comes back empty, and parse_expr_ref's sanity assert fires -
           // which is the trap `mv` and the closure literal below already document
           cursor.is_type(Token::Type::t_weak) ||
           cursor.is_type(Token::Type::t_strong) ||
           // `const(E)` - a value the compiler is required to work out. named for the reason the closure
           // literal below is: this and the production that reads one must agree on what admits it
           Parser::starts_const_expr(cursor) ||
           // `mv E` is a prefix operator, so it begins one too. without this the shunting-yard loop
           // never enters and the expression comes back empty
           cursor.is_type(Token::Type::t_mv) ||
           // a closure literal, `function(...) { ... }`. the same trap as `mv` above: without this the
           // loop does not enter and the expression comes back empty. guarded on the `(` so the
           // callable *type* `function<...>` - which is not an expression - cannot get in here
           Parser::starts_closure_literal(cursor) ||
           // `.ok(5)` - the shorthand static call, whose owner its destination names. exactly the trap
           // `mv` and the closure literal above document: without this the loop never enters, expr_parts
           // comes back empty and parse_expr_ref's sanity assert takes the compiler down with no
           // location at all. guarded on the whole shape by the predicate, so `..` - two t_dot, matched
           // as a declared infix symbol by the last arm - is untouched
           Parser::starts_shorthand_call(cursor) ||
           // `match ($u) { ... }` - an expression that produces the value one of its arms chose. the
           // same trap `mv`, the closure literal and the shorthand above document: without this the
           // shunting-yard loop never enters, expr_parts comes back empty and parse_expr_ref's sanity
           // assert takes the compiler down with no location at all
           Parser::starts_match(cursor) ||
           // `$a instanceof Foo` continues an expression that already began, so the loop must not
           // stop at the keyword - parse_postfix_chain is what actually consumes it
           cursor.is_type(Token::Type::t_instanceof) ||
           cursor.is_type(Token::Type::t_open_bracket) ||
           // if the token has a operator precendence, it is a valid expression token
           AST::Operator::get_precedence_for_token(cursor.current().type()).sequence > 0 ||
           // a prefix-only symbol, which the test above cannot answer for: it carries no precedence tier,
           // there being nothing to order it against. without this the loop below never enters for
           // `if (!$b)`, expr_parts holds nothing but the `(` and parse_expr_ref's single-part reporter
           // fires on an operator with no operand
           AST::Operator::is_prefix_only_token(cursor.current().type()) ||
           // a declared operator in **prefix or infix** position, either of which may be spelled out of
           // tokens nothing else in this list admits. a prefix one *begins* an expression - `!!` is two
           // t_exclamation and neither carries a precedence, so without it the loop below never enters
           // for `echo !!'hello';`, `expr_parts` stays empty and the sanity assert at the end of
           // parse_expr_ref takes the compiler down. an infix one *continues* one, the same gap on the
           // other side: `..`, the range operator, would otherwise end the expression at its first
           // token and `0 .. 10` reports an unexpected `.`
           //
           // gated to those two fixities so a bare identifier - which is already admitted above, as the
           // start of a call - does not change meaning just because some suffix operator is spelled
           // that way somewhere in the program. neither arm changes anything for a symbol spelled as a
           // word, and the shunting yard's `expects_operand` still refuses an infix one written in
           // operand position
           //
           // **last**, because it is the only arm that costs a lookup: the precedence test above is a
           // switch on the token type and answers for every built-in operator token, so only a token
           // that is nothing else in this list reaches the symbol trie
           starts_declared_operator_in_expression(payload, cursor);
}

// `( expr )`, the shape every call-like form here is written in - `weak(...)`, `strong(...)`, a pointer
// cast. one helper rather than three copies, so the recovery is the same wherever the parens are: a
// located issue at whichever bracket was missing, and nullptr for the caller to recover from
AST::ExprNode *parse_parenthesized_operand(Parser::Payload &payload, AST::TypeNode *expected_type = nullptr)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type(Token::Type::t_open_paren)) {
        payload.collect_unexpected_token(Token::Type::t_open_paren);
        return nullptr;
    }
    cursor.skip();

    auto *operand = Parser::parse_expr(payload, expected_type);
    if (operand == nullptr) {
        return nullptr;
    }

    if (!cursor.is_type(Token::Type::t_close_paren)) {
        payload.collect_unexpected_token(Token::Type::t_close_paren);
        return nullptr;
    }
    cursor.skip();

    return operand;
}

AST::ExprNode *Parser::parse_weak_expr(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;
    auto weak_token = cursor.current();

    // the explicit form goes through **Parser::parse_type**, the one owner of the type grammar, rather
    // than re-reading `< type >` here. so `weak<a::b::Foo>` and `weak<Box<int32>>` mean the same in an
    // expression as in a declaration, including the `>>` split, and the class-only rule is enforced once
    std::optional<AST::ValueType> written_target;
    if (cursor.is_type_sequence(0, { Token::Type::t_weak, Token::Type::t_open_angle })) {
        AST::TypeNode *written = Parser::parse_type(payload);
        if (written == nullptr) {
            return nullptr;
        }

        // parse_type already reported anything that is not a weak - a `weak<int32>` collects its issue at
        // the type and answers nullopt there, so reaching here with something else is not possible
        if (!written->type.is_weak()) {
            return nullptr;
        }

        written_target = written->type.weak_target();
    }
    else {
        cursor.skip(); // `weak`
    }

    auto *operand = parse_parenthesized_operand(payload);
    if (operand == nullptr) {
        return nullptr;
    }

    const AST::ValueType operand_type = operand->result_type();

    // a weak reference needs the object's *handle*, which means somewhere to read it from. a temporary
    // would be released the moment the statement ended, so the weak would be dead before it was named -
    // refusing here says that, rather than letting the program discover it at runtime
    if (!AST::is_place_expression(*operand)) {
        payload.collector.collect_issue<AST::Issue::AddressOfTemporary>(
            payload.context.code_ref(weak_token),
            "'weak' needs an expression with storage to reference - a value nothing owns is already "
            "gone by the time a weak reference to it could be read");
        return nullptr;
    }

    // an undetermined operand is "ask again later", the standing rule for a type no round has answered.
    // the node's own result_type() re-asks after substitution, so a `&$t` inside a template needs no
    // decision here either
    if (!operand_type.is_class() && !AST::is_undetermined_type(operand_type)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(weak_token),
            fmt::format(
                "'weak' needs a class, not '{}' - only a class is reference counted, so only a class "
                "has a count to opt out of",
                operand_type.get_type_desciption()));
        return nullptr;
    }

    // checked against the operand, never used to change it: the target of a weak reference is whatever
    // the operand already is. so a mismatch is a claim the program got wrong rather than a conversion
    if (written_target.has_value() && operand_type.is_class()
        && !(written_target.value() == operand_type)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(weak_token),
            fmt::format(
                "'weak<{}>' does not match its operand's type '{}' - a weak reference names the class "
                "it was taken of, so the two cannot differ",
                written_target->get_type_desciption(), operand_type.get_type_desciption()));
        return nullptr;
    }

    // **the same node a `&` with a weak destination builds.** `weak($obj)` is not a second operation, it is
    // the same one saying so itself instead of reading it off a destination - so ownership, lowering and the
    // dump all follow one path and cannot come apart. it is also the spelling for the case the destination
    // rule cannot serve: an *inferred* `$w = weak($a)` has no declared type to ask
    return &payload.context.emplace_node<AST::AddrOfExprNode>(operand, true);
}

AST::ExprNode *Parser::parse_strong_expr(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;
    auto strong_token = cursor.current();
    cursor.skip(); // `strong`

    auto *operand = parse_parenthesized_operand(payload);
    if (operand == nullptr) {
        return nullptr;
    }

    // no place requirement and no type check here: the operand is read as a *value*, so a weak a call
    // returned upgrades as readily as one in a variable, and the "is it a weak" question belongs to
    // AST::TypeChecker - which asks it after the monomorphizer has settled every type
    return &payload.context.emplace_node<AST::StrongExprNode>(operand, strong_token);
}

namespace
{
    // what the shape scan below found: where the owner's spelling ends, which identifier names the
    // type, and the namespace segments written ahead of it. the last two are what let the owner be
    // *resolved* before any of it is parsed - see try_parse_static_call
    struct StaticCallShape
    {
        size_t split = 0;
        size_t name_offset = 0;
        std::vector<std::string> namespace_parts;
    };

    // **where the owner's spelling ends in `Type::f(`** - the index of the `::` that separates the type
    // from the static's name, or nullopt when these tokens are not a static call at all.
    //
    // scanned rather than parsed because the two readings of one token run - `a::b::f()` as a namespaced
    // free call and `A::B::f()` as a static on a nested type - are told apart by what the *names* denote
    // and not by their shape. so the shape is measured first, cheaply, and only a run that could be either
    // is looked up. the last `::` wins: everything before it is the owner, which is what makes
    // `game::Session::active()` name a type in a namespace rather than a namespace of that name
    std::optional<StaticCallShape> static_call_split(Parser::Cursor &cursor, bool want_property)
    {
        // the cheap early-out: an owner is a type name, so it opens with an identifier and continues
        // with either a `::` or the `<` of a generic application. anything else cannot be one, and this
        // runs at every operand position
        if (!cursor.is_type(Token::Type::t_identifier)
            || !(cursor.peek_is_type(1, Token::Type::t_namespace_sep)
                || cursor.peek_is_type(1, Token::Type::t_open_angle))) {
            return std::nullopt;
        }

        std::optional<StaticCallShape> found;
        const size_t base = cursor.snapshot().index;

        // every identifier walked so far. when a split is recorded, the one at that iteration names the
        // owner and the ones before it are the namespace it is written in - which is what
        // try_parse_static_call resolves before it parses anything
        std::vector<std::string> walked;

        // **walks the qualified name and stops where it ends** - it does not scan for a `::` anywhere
        // ahead. an unbounded search reads the next statement's `::` as this expression's, which turns
        // any `a::b()` in a file that later mentions a type into a mis-split
        size_t offset = 0;

        while (cursor.peek_is_type(offset, Token::Type::t_identifier)) {
            const std::string name = cursor.peek(offset).value();
            const size_t name_offset = base + offset;
            // a generic owner: `result<int32, E>::ok(...)`. the angles are counted rather than matched
            // by a parser, this being a shape measurement - and `>>` closes two, which is the same
            // token the type grammar splits by hand
            if (cursor.peek_is_type(offset + 1, Token::Type::t_open_angle)) {
                size_t depth = 0;
                size_t scan = offset + 1;

                // **bounded by the statement, not by the file.** a `<` that opens no type argument list
                // is the common case here - `MAX < $n` reaches this on exactly the shape an owner does,
                // a compile-time constant being a bare identifier - and an unbounded scan for a closer
                // that is not coming walks every remaining token in the file, once per such comparison.
                // parse_explicit_type_args already had this and was already fixed; a type argument list
                // cannot span a `;`, so that is the honest bound
                for (; cursor.is_valid_offset(scan) && !cursor.peek_is_type(scan, Token::Type::t_semicolon); scan++) {
                    if (cursor.peek_is_type(scan, Token::Type::t_open_angle)) {
                        depth++;
                    }
                    else if (cursor.peek_is_type(scan, Token::Type::t_close_angle)) {
                        depth--;
                        if (depth == 0) break;
                    }
                    else if (cursor.peek_is_type(scan, Token::Type::t_op_shr)) {
                        if (depth <= 2) { depth = 0; break; }
                        depth -= 2;
                    }
                }

                if (depth != 0 || !cursor.is_valid_offset(scan)) {
                    break;
                }

                offset = scan + 1;
            }
            else {
                offset++;
            }

            // not a `::`, so the qualified name ended at the identifier before it - and this was a
            // plain call, a constant or a comparison rather than anything with an owner
            if (!cursor.peek_is_type(offset, Token::Type::t_namespace_sep)) {
                break;
            }

            // **what may follow the `::` is the whole of what tells the two forms apart**: a `$name` is
            // a static property and there is nothing else it could be, while a call's name is an
            // identifier - and then, for everything but an enum's case, a `(` or the `<` of a type
            // argument list. the angle is enough to record the split - whether the list closes is
            // parse_funccall's speculation to run, and it puts the tokens back untouched when it declines
            //
            // **a bare `Type::name` is recorded too, and only try_parse_static_call can say what it is.**
            // `DistanceUnit::meter` is a case, `Point::MAX` is a constant, and `std::math::PI` is a
            // constant in a namespace - all three the same three tokens, told apart by what the *owner*
            // declares and not by what follows. so the shape scan widens and the commit narrows: a name
            // whose owner is not a type never reaches an owner at all, and one whose owner declares no
            // such case is put back for the constant path to read
            //
            // the *last* such `::` in the run wins, which is what lets `game::Session::active()` name
            // a type inside a namespace rather than a namespace called Session
            const bool follows = want_property
                ? cursor.peek_is_type(offset + 1, Token::Type::t_varname)
                : cursor.peek_is_type(offset + 1, Token::Type::t_identifier);

            if (follows) {
                found = StaticCallShape { base + offset, name_offset, walked };
            }

            walked.push_back(name);
            offset++; // past the `::`
        }

        return found;
    }
}

bool Parser::starts_static_property(Parser::Cursor &cursor)
{
    return static_call_split(cursor, /*want_property=*/true).has_value();
}

namespace
{
    // **the owner both static forms are written after**, or nothing - with the cursor left just past
    // the `::` on success and exactly as it was found on every failure.
    //
    // one function because `Type::f(...)` and `Type::$x` differ only in what comes *after* the `::`:
    // the owner is the same grammar, resolved the same way, and speculating on it has the same rule -
    // report nothing, since a name that turns out to be a namespace is not a mistake
    AST::TypeNode *parse_static_owner(Parser::Payload &payload, const StaticCallShape &shape)
    {
        auto &cursor = payload.cursor;

        // **the owner is resolved before it is parsed**, and that order is the whole of what keeps this
        // speculation silent. Parser::parse_type *reports* an unresolved qualified name - it has to,
        // since parse_namespace mints what it does not find and a quiet failure there would be an
        // unknown type nobody mentioned - so handing it `std::math` out of `std::math::sqrt(16.0)`
        // costs a diagnostic for a spelling that is not an error at all
        //
        // one symbol lookup instead, on exactly the name the shape scan says is the owner: outward
        // from the enclosing namespace for a bare name, the way every use site resolves one, and exact
        // within a written namespace path for a qualified one. a path that does not exist answers null
        // rather than being created, which is the second half of staying silent
        {
            const auto &parts = shape.namespace_parts;
            const std::string owner_name = cursor.tokens[shape.name_offset].value();

            const AST::Namespace *in = payload.context.current_namespace;

            if (!parts.empty()) {
                in = payload.collector.namespaces.get(parts);

                if (in == nullptr) {
                    return nullptr;
                }
            }

            auto *symbol = parts.empty()
                ? payload.collector.namespaces.find_symbol_in_scope(owner_name, *in)
                : payload.collector.namespaces.find_symbol(owner_name, *in);

            if (symbol == nullptr || symbol->type() != AST::SymbolType::t_type) {
                return nullptr;
            }
        }

        const auto start = cursor.snapshot();

        // **the owner is read by the real type grammar, bounded to its own tokens.** narrowing the
        // cursor's end is what lets parse_type answer here at all: unbounded it would read
        // `Point::norm` as a nested type, report that `Point` has no nested `norm`, and leave a
        // diagnostic behind for a spelling that is not an error. bounded, the `::` is simply past the
        // end and the type ends where it should
        //
        // and it is parse_type rather than a walk of identifiers because the owner is a *type*:
        // `Box<int32>`, `game::Session`, `string::view` are all one grammar, and a second one here
        // would drift from it
        auto narrowed = start;
        narrowed.end = shape.split;
        cursor.restore(narrowed);

        if (!Parser::can_parse_type(payload)) {
            cursor.restore(start);
            return nullptr;
        }

        AST::TypeNode *owner_node = Parser::parse_type(payload);

        // the owner did not parse, or did not consume its whole run - `a::b` where those are
        // namespaces reaches here, and must go back untouched for the namespace walk to read. nothing
        // is reported: this is speculation, and a name that is not a type is not yet a mistake
        const bool consumed_owner = cursor.is_done();

        if (owner_node == nullptr || !consumed_owner || !owner_node->type.has_complex_type()) {
            cursor.restore(start);
            return nullptr;
        }

        // back to the real range, keeping the position: the owner is behind us, the `::` is next
        auto resumed = cursor.snapshot();
        resumed.end = start.end;
        cursor.restore(resumed);

        cursor.skip(); // the `::` the split named

        return owner_node;
    }
}

AST::TypeNode *Parser::try_parse_static_owner(Parser::Payload &payload, bool want_property)
{
    const auto shape = static_call_split(payload.cursor, want_property);

    if (!shape.has_value()) {
        return nullptr;
    }

    return parse_static_owner(payload, shape.value());
}

AST::FunctionCallExprNode *Parser::try_parse_static_call(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    const auto start = cursor.snapshot();
    const AST::TypeNode *owner_node = Parser::try_parse_static_owner(payload, /*want_property=*/false);

    if (owner_node == nullptr) {
        return nullptr;
    }

    const AST::ValueType owner = owner_node->type;

    // **a nested type wins over a static of the same name**, and it has to be asked before committing:
    // `string::view($b, $n)` is a *constructor* call, which resolves through the member surface
    // namespace exactly as it always has. the two spellings are identical, so the only thing that can
    // tell them apart is what the owner declares - and a nested type is the older meaning
    //
    // asked of the template, a nested type inside a generic owner being refused where it is declared
    if (cursor.is_type(Token::Type::t_identifier)
        && owner.get_complex_type()->template_or_self()->find_member_type_decl(cursor.current().value()) != nullptr) {
        cursor.restore(start);
        return nullptr;
    }

    // **`DistanceUnit::meter` with no argument list, which only an enum's case may be.**
    //
    // read as the call that builds it rather than as a form of its own: a case *is* a static function
    // of no arguments, so the paren-free spelling is sugar and nothing downstream - resolution, the
    // monomorphizer, ownership, codegen - learns that it existed. `-p ast-resolved` shows the call,
    // which is the same bargain a drop and an implicit conversion already make
    //
    // asked here rather than in the shape scan because it is a question about the *owner*: the scan saw
    // three tokens that `Point::MAX` and `std::math::PI` spell identically, and what tells them apart is
    // that this owner is an enum and declares this case. anything else goes back untouched, which is
    // what keeps the constant path reading exactly what it read before
    if (!cursor.peek_is_type(1, Token::Type::t_open_paren)
        && !cursor.peek_is_type(1, Token::Type::t_open_angle)) {
        const AST::ComplexType *ct = owner.get_complex_type()->template_or_self();

        if (!cursor.is_type(Token::Type::t_identifier)
            || !ct->is_enum_kind()
            || ct->find_enum_case(cursor.current().value()) == nullptr) {
            cursor.restore(start);
            return nullptr;
        }

        const TokenReference case_token = cursor.current();
        cursor.skip();

        auto &call = payload.context.emplace_node<AST::FunctionCallExprNode>(
            case_token, std::vector<AST::ExprNode *>{});
        call.static_owner = owner;

        return &call;
    }

    // **committed.** from here the owner is known to be a type, so a name it does not declare is a
    // real diagnostic and not a reason to try the namespace path - falling through at this point is
    // what would recreate the outward walk that lets `Foo::f()` mean an enclosing free `f`
    bool is_call = false;
    auto *call = parse_funccall(payload, nullptr, &is_call, Parser::CallLookup { nullptr, owner, std::nullopt });

    if (call != nullptr) {
        return call;
    }

    // parse_funccall declined the speculative `<` - so `Point::MAX < $n` is a comparison, not a call
    // on a type. put everything back and let the constant path read it
    if (!is_call) {
        cursor.restore(start);
    }

    return nullptr;
}

AST::StaticPropertyExprNode *Parser::try_parse_static_property(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    const AST::TypeNode *owner_node = Parser::try_parse_static_owner(payload, /*want_property=*/true);

    if (owner_node == nullptr) {
        return nullptr;
    }

    const AST::ValueType owner = owner_node->type;
    const auto name_token = cursor.current();

    cursor.skip(); // the `$name`

    // **committed from here**, exactly as the call form is, and for the same reason: `Type::$x` where
    // `Type` is a type has no second reading to fall back to. a namespace has no `$` names in it
    const AST::ComplexType *owner_type = owner.get_complex_type()->template_or_self();
    const auto found = owner_type->find_static_property(name_token.value());

    if (!found.has_value()) {
        payload.collector.collect_issue<AST::Issue::UnknownStaticProperty>(
            payload.context.code_ref(name_token),
            name_token.value(),
            owner.get_type_desciption()
        );

        // **reported, and then a node with no declaration is handed back anyway.** the alternatives
        // both cost a second, unrelated diagnostic: recovering to the next statement leaves
        // parse_varexpr mid-declaration, and returning null sends these tokens to the namespace walk,
        // which mints a namespace and then meets a `$name` no operand arm accepts
        //
        // a null `decl` is a legitimate hole - result_type() answers `unknown` for it, ensure_static_init
        // declines it, and codegen never sees it because has_critical_issues() already stopped
        return &payload.context.emplace_node<AST::StaticPropertyExprNode>(
            name_token, owner, nullptr, 0);
    }

    // **a `private` static is reachable only from inside its own type**, which is the same rule and the
    // same question an instance property answers - AST::can_reach_private_member, off the enclosing
    // declaration rather than off the file
    if (found->second->is_private
        && !AST::can_reach_private_member(
            payload.context.self_struct_ptr != nullptr
                ? &payload.context.self_struct_ptr->complex_type()
                : nullptr,
            owner_type)) {
        payload.collector.collect_issue<AST::Issue::PrivateMember>(
            payload.context.code_ref(name_token),
            name_token.value(),
            owner.get_type_desciption()
        );
    }

    auto &node = payload.context.emplace_node<AST::StaticPropertyExprNode>(
        name_token, owner, found->second, found->first);

    return &node;
}

const AST::NodeReference Parser::parse_postfix_chain(Parser::Payload &payload, AST::NodeReference base)
{
    auto &cursor = payload.cursor;
    auto current_ref = base;

    // **the `?->` links opened in this chain, innermost last.** each one swaps the marker in for the base
    // and records what it has to wrap; the whole lot is unwound after the loop, in reverse
    //
    // deferred rather than wrapped on the spot, and that is what makes `$a?->b->c` mean what a reader
    // expects: everything after the `?->` becomes part of *one* short circuit, so an absent `$a` skips the
    // `->c` as well. wrapping immediately would have left `->c` applied to a `B?`, which is refused - and
    // would have forced a `?->` at every link whether or not that link could be absent
    struct PendingOptionalLink
    {
        AST::ExprNode *base;
        AST::ChainBaseNode *marker;
        TokenReference token;
    };
    std::vector<PendingOptionalLink> optional_links;

    // one loop for every suffix that binds tighter than any operator. `->`, `?->` and `:$` live here
    // rather than in the shunting yard, which has no notion of a postfix operator and pops two
    // operands for everything it sees. `[...]` joins them later
    while (cursor.is_type(Token::Type::t_accessorlr)
        || cursor.is_type(Token::Type::t_optional_arrow)
        || cursor.is_type(Token::Type::t_ptr_of)
        || cursor.is_type(Token::Type::t_instanceof)
        || cursor.is_type(Token::Type::t_open_bracket)) {

        // `?->` opens a short circuit and then reads a member exactly as `->` does. so it is handled by
        // *substitution* rather than by a second member-access implementation: the base is replaced with a
        // marker standing for its unwrapped self, and the `->` arm below carries on unchanged
        if (cursor.is_type(Token::Type::t_optional_arrow)) {
            auto optional_token = cursor.current();
            auto *base = current_ref.unsafe_ptr<AST::ExprNode>();

            // the weak upgrade, through the one function all three forms share
            base = AST::optional_operand_of(base, payload.context.module, optional_token);

            const AST::ValueType base_type = base->result_type();

            // a base that is always there makes the `?` a lie - the short circuit could never fire, and
            // the reader is being told to expect an absence that cannot happen. `->` is what they want
            const std::string refusal =
                AST::certainly_present_refusal(AST::OptionalForm::t_optional_chain, base_type);

            if (!refusal.empty()) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(optional_token), refusal);
                return AST::make_void_ref();
            }

            auto &marker = payload.context.emplace_node<AST::ChainBaseNode>(
                AST::unwrapped_type_of(base_type), optional_token);

            optional_links.push_back({base, &marker, optional_token});

            // from here the loop reads an ordinary `->` off the marker. the token is *not* consumed:
            // the arm below consumes it, and both spellings mean the same thing to it
            current_ref = AST::make_ref(marker);
        }

        // `E instanceof T`. here rather than in the shunting yard for a reason the other suffixes only
        // share by accident: its right operand is a *type*, not an expression, so there is no operand
        // for the yard to pop. binding as tightly as `->` also makes `$a instanceof Foo == true` read
        // the way it looks, with the comparison applied to the answer
        if (cursor.is_type(Token::Type::t_instanceof)) {
            auto instanceof_token = cursor.current();
            cursor.skip();

            auto *queried = Parser::parse_type(payload);
            if (queried == nullptr) {
                return AST::make_void_ref();
            }

            auto &check = payload.context.emplace_node<AST::InstanceOfExprNode>(
                current_ref.unsafe_ptr<AST::ExprNode>(), queried->type, instanceof_token);

            current_ref = AST::make_ref(check);
            continue;
        }

        if (cursor.is_type(Token::Type::t_open_bracket)) {
            auto bracket_token = cursor.current();
            cursor.skip();

            auto *base = current_ref.unsafe_ptr<AST::ExprNode>();

            // **an element is addressed, so the container has to have storage or be able to be given
            // some.** a place has it; `$p:$` names an address directly; and anything materializable can
            // be bound to a temporary and indexed out of that, which is what makes `make()[0]` read the
            // way it looks. the container is operand 0 of the element call, so it is an
            // ordinary borrow argument from there on and AST::argument_fit ranks it t_borrow_temporary
            //
            // what is left is genuinely addressless - a bare `null`, an array literal - and the message
            // is the one the shunting yard's fallback still gives `5[0]` and `[1, 2][0]`, which no
            // postfix chain claims. nearly the method-receiver gate in FuncCallParser, and for the same
            // reason - a receiver *is* a borrow argument in position 0 - **but for `:$`**, which is
            // addressless there and admitted here. a `->` base is read by parse_postfix_chain, which
            // never hands one over, so the two gates have not had to answer it in the same terms yet
            if (AST::storage_of(*base) == AST::StorageClass::t_addressless
                && base->get_node_type() != AST::NodeType::n_expr_peel) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(bracket_token),
                    "only a place can be indexed - a variable, a field or an element. Bind this "
                    "expression to a name first, then index that.");
                return AST::make_void_ref();
            }

            // **an empty `[]` is legal here**, and means the slot after the last one. it is not read
            // as "an index that failed to parse": AST::OperatorRewriter resolves it against the
            // container's one-operand `operator []`, and Parser refuses it anywhere but an
            // assignment target - see AST::IndexExprNode::is_append
            std::vector<AST::ExprNode *> indices;

            if (!parse_bracketed_expr_list(payload, indices)) {
                return AST::make_void_ref();
            }

            auto &index_expr = payload.context.emplace_node<AST::IndexExprNode>(
                base, std::move(indices), bracket_token);

            // the `:$` marker is erased by AST::PointerAdjuster long before the pass that has to
            // know, so whether one was written is recorded here, where it is still in the tree
            index_expr.base_was_peeled = base->get_node_type() == AST::NodeType::n_expr_peel;

            current_ref = AST::make_ref(index_expr);
            continue;
        }

        if (cursor.is_type(Token::Type::t_ptr_of)) {
            auto peel_token = cursor.current();
            cursor.skip();

            auto *operand = current_ref.unsafe_ptr<AST::ExprNode>();

            // `:$` walks one level outward each time. applied to an already peeled expression
            // there is no transparency left to strip, so it means the address of the slot -
            // which makes `$out:$:$` identical to `&$out` rather than a special case
            // (`$out:$:$` is `&$out`)
            if (operand->get_node_type() == AST::NodeType::n_expr_peel) {
                // no weak reading here, and it needs no destination to decide: `:$` requires a pointer
                // operand, and a class handle is not one - so `$out:$:$` is always the slot's address,
                // which is exactly the `&$out` it is documented to be identical to
                auto &addr = payload.context.emplace_node<AST::AddrOfExprNode>(
                    static_cast<AST::PointerValueNode *>(operand)->operand);
                current_ref = AST::make_ref(addr);
                continue;
            }

            if (!AST::is_place_expression(*operand)) {
                payload.collector.collect_issue<AST::Issue::AddressOfTemporary>(
                    payload.context.code_ref(peel_token),
                    "':$' needs an expression with storage to reach the pointer of");
                return AST::make_void_ref();
            }

            auto &peel = payload.context.emplace_node<AST::PointerValueNode>(operand, peel_token);
            current_ref = AST::make_ref(peel);
            continue;
        }

        auto accessor_token = cursor.current();
        cursor.skip(); // skip the '->' token

        // `->` already reaches through every pointer level, so `$p:$->x` could only ever mean what
        // `$p->x` means. it is rejected rather than aliased: `:$` marks an operation *on the
        // address*, and the pointer object itself has no members
        // (`:$` names the pointer, which has no members)
        if (current_ref.type() == AST::NodeType::n_expr_peel) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(accessor_token),
                "':$' names the pointer itself, which has no members - drop the ':$' and write '->' directly");
            return AST::make_void_ref();
        }

        if (!cursor.is_type(Token::Type::t_identifier)) {
            payload.collect_unexpected_token(Token::Type::t_identifier);
            return AST::make_void_ref();
        }

        auto member_token = cursor.current();
        cursor.skip(); // skip the member name

        // `->name(` where `name` is a *property* of callable type is a call through that value, not a
        // method call: `$h->op(21)` on a `function<int32(int32)> $op`.
        //
        // tried before the member-call attempt below, and gated on the type having no member function of
        // that name, so no existing call changes meaning. it has to come first rather than as a fallback,
        // because parse_member_call *commits* once a `(` follows - it reports an unknown member instead of
        // restoring, which is the right behaviour for a name that was meant to be a method
        if (cursor.is_type(Token::Type::t_open_paren)) {
            const AST::ValueType base_type =
                AST::target_type_of(current_ref.unsafe_ptr<AST::ExprNode>()->result_type());

            // the property is asked for first even though the member-function gate is the more
            // interesting condition: find_property is an O(1) map hit, where find_member_functions is a
            // linear scan that builds a vector. `->name(` is overwhelmingly an ordinary method call, and
            // in that shape there is no callable property, so this way it never pays for the scan
            const AST::ComplexType::Property *property = base_type.has_property_layout()
                ? base_type.get_complex_type()->find_property(member_token.value())
                : nullptr;

            if (property != nullptr
                && property->type.has_signature()
                && AST::find_member_functions(base_type.get_complex_type(), member_token.value()).empty())
            {
                auto &member_access =
                    payload.context.emplace_node<AST::MemberAccessNode>(current_ref, member_token);

                auto *call = Parser::parse_indirect_call(payload, &member_access, member_token);

                if (call == nullptr) {
                    return AST::make_void_ref();
                }

                current_ref = AST::make_ref(*call);
                continue;
            }
        }

        // `->name(` is a method call rather than a member read. `->name<` may be either: unlike a
        // free call, where a bare identifier can never be a comparison operand because values carry
        // a `$`, a member *is* a legitimate operand and `$a->count < 3` has to keep working. so the
        // type argument list is parsed speculatively and only committed when a `(` follows it
        if (cursor.is_type(Token::Type::t_open_paren) || cursor.is_type(Token::Type::t_open_angle)) {
            bool is_call = false;
            auto *call = Parser::parse_member_call(
                payload, current_ref.unsafe_ptr<AST::ExprNode>(), member_token, is_call);

            if (call != nullptr) {
                current_ref = AST::make_ref(*call);
                continue;
            }

            // a call that failed to resolve has already reported. reinterpreting its tokens as a
            // member read would report the same name a second time, from the type checker
            if (is_call) {
                return AST::make_void_ref();
            }

            // otherwise the `<` was a comparison: the cursor is back on it and this falls through to
            // the member read below, which is what `$a->count < 3` needs
        }

        auto &member_access = payload.context.emplace_node<AST::MemberAccessNode>(current_ref, member_token);
        current_ref = AST::make_ref(member_access);
    }

    // **the short circuits close here, innermost first.** everything the loop built after a `?->` is that
    // link's continuation, so unwinding in reverse puts each chain node around exactly the part of the
    // expression its base guards - and `$a?->b?->c` nests, stopping at whichever link is absent first
    for (auto link = optional_links.rbegin(); link != optional_links.rend(); ++link) {
        auto *continuation = current_ref.unsafe_ptr<AST::ExprNode>();

        auto &chain = payload.context.emplace_node<AST::OptionalChainExprNode>(
            link->base,
            continuation,
            link->marker,
            link->token,
            AST::optional_chain_result_type(continuation, payload.collector.type_registry));

        current_ref = AST::make_ref(chain);
    }

    return current_ref;
}

// consumes every declared **suffix** operator following an operand, innermost first, so
// `1m + 50cm` reads its units before the addition and `"x"_handle` is the whole operand
//
// here rather than in the shunting yard for parse_postfix_chain's reason - the yard pops two operands
// for everything it sees - and its own function rather than an arm of that chain because it has a
// second caller: a literal does not route through the postfix chain, and `1m` needs it to
//
// binding tighter than every binary operator is what the chapter's "the precedence of unary operators
// is higher than that of binary operators" means, and it is structural here rather than a number
const AST::NodeReference parse_suffix_operator_chain(Parser::Payload &payload, AST::NodeReference base)
{
    auto &cursor = payload.cursor;
    auto current_ref = base;

    while (current_ref.has()) {
        // one match per turn: the loop both decides on the symbol and consumes it, and those were two
        // separate lookups of the same position
        const auto match = declared_operator_at(payload, cursor, AST::OpFixity::t_suffix);

        if (!match.has()) {
            break;
        }

        const TokenReference symbol_token = cursor.current();

        auto *operand = current_ref.unsafe_ptr<AST::ExprNode>();
        if (operand == nullptr) {
            return AST::make_void_ref();
        }

        cursor.skip(match.token_count);

        auto *call = Parser::build_operator_call(
            payload, *match.op, AST::OpFixity::t_suffix, symbol_token, {operand});

        if (call == nullptr) {
            return AST::make_void_ref();
        }

        current_ref = AST::make_ref(*call);
    }

    return current_ref;
}

// `&name` - a C function pointer. the grammar is free: a glued `&` cannot be a
// variable, variables carry `$`. spelling refusals stay here (`&f(...)` is a
// call result; `&CONST` is the bitwise-and hint). resolution does not: a unique
// candidate is bound even when it cannot be addressed, an empty set is left
// unresolved, and TypeChecker is the only reporter of either
const AST::NodeReference parse_function_ref(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;

    const TokenReference amp = cursor.current();
    cursor.skip();

    const auto start = cursor.snapshot();
    AST::TypeNode *owner = Parser::try_parse_static_owner(payload, /*want_property=*/false);

    const AST::Namespace *ns = payload.context.current_namespace;
    bool qualified = false;
    AST::ValueType static_owner;

    if (owner != nullptr && cursor.is_type(Token::Type::t_identifier)) {
        static_owner = owner->type;
    }
    else {
        cursor.restore(start);

        if (cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_namespace_sep })) {
            auto *ns_node = parse_namespace(payload);
            ns = ns_node != nullptr ? ns_node->ast_namespace : ns;
            qualified = true;
        }

        if (!cursor.is_type(Token::Type::t_identifier)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(amp),
                "'&' expected a function name.");
            cursor.try_skip_to_next_statement();
            return AST::make_void_ref();
        }
    }

    auto &ref = payload.context.emplace_node<AST::FunctionRefExprNode>(amp, cursor.current());
    ref.lookup_namespace = ns;
    ref.is_qualified = qualified;
    ref.static_owner = static_owner;

    cursor.skip();

    const auto candidates = AST::function_ref_candidates(ref, payload.collector.functions);

    if (!ref.is_static()
        && AST::find_constant(
            payload.collector.namespaces,
            ref.token_name.value(),
            ns,
            qualified) != nullptr
        && candidates.empty())
    {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(amp),
            fmt::format(
                "a constant has no address - '&{}' cannot be taken. Write '& {}' with a "
                "space if you meant bitwise and.",
                ref.token_name.value(),
                ref.token_name.value()));
        ref.resolved = true;
        return AST::make_ref(ref);
    }

    // bind a unique candidate even when it cannot be addressed: TypeChecker reports the
    // refusal against a chosen name rather than an empty set
    if (candidates.size() == 1) {
        ref.decl = candidates[0];
        ref.resolved = true;
    }

    if (expected_type != nullptr) {
        AST::bind_function_ref_to(&ref, expected_type->type, payload.collector.functions);
    }

    auto chained = Parser::parse_postfix_chain(payload, AST::make_ref(ref));

    // `&add(41)` is the function-ref plus a postfix call, the same `$f(...)` shape a
    // variable of callable type already has. parse_postfix_chain does not consume `(`
    if (cursor.is_type(Token::Type::t_open_paren) && chained.is_expression_node()) {
        if (auto *call = Parser::parse_indirect_call(
                payload, chained.unsafe_ptr<AST::ExprNode>(), amp)) {
            return Parser::parse_postfix_chain(payload, AST::make_ref(*call));
        }
    }

    return chained;
}

const AST::NodeReference parse_expr_node(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;

    if (cursor.is_type(Token::Type::t_floating_literal)) {
        return parse_literal_float(payload, expected_type);
    }

    else if (cursor.is_type(Token::Type::t_integer_literal)) {
        return parse_literal_int(payload, expected_type);
    }

    else if (cursor.is_type(Token::Type::t_hex_literal)) {
        return parse_literal_hex(payload, expected_type);
    }

    else if (cursor.is_type(Token::Type::t_binary_literal)) {
        return parse_literal_binary(payload, expected_type);
    }

    else if (cursor.is_type(Token::Type::t_bool_literal)) {
        return parse_literal_boolean(payload, expected_type);
    }

    // `[1, 2, 3]`. the elements are parsed with no expected type of their own: each one is typed
    // where it lands, by the append the rewriter expands this into, which is an ordinary assignment
    // into a `T` slot - so `autocast_literal_int` and `coerce_value` do the work they already do
    else if (cursor.is_type(Token::Type::t_open_bracket)) {
        const auto bracket_token = cursor.current();
        cursor.skip();

        std::vector<AST::ExprNode *> elements;

        if (!parse_bracketed_expr_list(payload, elements)) {
            return AST::make_void_ref();
        }

        auto &literal = payload.context.emplace_node<AST::ArrayLiteralExprNode>(
            std::move(elements), bracket_token);

        return AST::make_ref(literal);
    }

    // `match ($u) { ... }` - the value one of its arms chose. an operand like any other from here on,
    // which is what lets it sit anywhere a value may: an argument, a return, the right of an `=`, or
    // alone as a statement
    else if (Parser::starts_match(cursor)) {
        AST::MatchExprNode *node = Parser::parse_match(payload);

        if (node == nullptr) {
            return AST::make_void_ref();
        }

        return Parser::parse_postfix_chain(payload, AST::make_ref(*node));
    }

    // **`.name(...)`, a static call whose owner its destination names.** beside the `null` arm below
    // because it is the same shape of thing: a value with no type of its own, which the place it is
    // going says. the difference is only which question the destination answers - `null`'s is "may
    // absence arrive here", this one's is "which type declares this"
    //
    // it is an ordinary FunctionCallExprNode with no owner rather than a node of its own, and that is
    // what makes the rest free: `result_type()` already answers `void` for a call with no decl, which
    // is_undetermined_type accepts - so argument_fit scores it t_undetermined at its first arm,
    // strictly_better skips it on both sides, and **a shorthand can never take part in choosing an
    // overload**, enforced by construction with no rule anywhere saying so
    else if (Parser::starts_shorthand_call(cursor)) {
        const auto dot_token = cursor.current();

        // `.cannot_connect` - a case of the enum the destination names, written without an argument
        // list because it has no payload. the paren-free `DistanceUnit::meter` read one line of
        // machinery differently: there the owner is written and the case can be checked against it
        // here and now, while here the owner is exactly what nothing has said yet - so this builds the
        // same undetermined shorthand the argument form does and lets AST::bind_shorthand_to and then
        // AST::CallResolver answer, which is where "does that type declare this name" already lives
        const bool has_arguments = Parser::shorthand_call_has_arguments(cursor);

        cursor.skip(); // the `.`

        if (!has_arguments) {
            const auto case_token = cursor.current();
            cursor.skip();

            auto &call = payload.context.emplace_node<AST::FunctionCallExprNode>(
                case_token, std::vector<AST::ExprNode *>{});
            call.token_shorthand_dot.emplace(dot_token);

            if (expected_type != nullptr) {
                AST::bind_shorthand_to(&call, expected_type->type);
            }

            return Parser::parse_postfix_chain(payload, AST::make_ref(call));
        }

        bool is_call = false;
        auto *call = parse_funccall(payload, nullptr, &is_call, Parser::CallLookup { nullptr, {}, dot_token });

        if (call == nullptr) {
            return AST::make_void_ref();
        }

        // the destination, where the position carries one it already knows: a declared variable's type
        // and a return type both arrive here. an *argument* cannot - the parameter sits on a
        // declaration nobody has chosen yet - so that one is bound by AST::CallResolver instead
        if (expected_type != nullptr) {
            AST::bind_shorthand_to(call, expected_type->type);
        }

        return Parser::parse_postfix_chain(payload, AST::make_ref(*call));
    }

    else if (cursor.is_type(Token::Type::t_null)) {
        // null has no type of its own - it takes the one the position expects. an unbound null
        // stays untyped here and is reported by the checker, which has the context to say so
        auto &node = payload.context.emplace_node<AST::NullNode>(cursor.current());

        // **any destination that admits absence**, which is one question rather than a list of kinds:
        // a `ptr<T>`, a `weak<T>`, and any `T?` whatever T is. `is_pointer() || is_class()` would
        // miss `int32? $x = null;` and leave it untyped at codegen, and would accept
        // `Foo $x = null;` whether the author asked for absence or not
        //
        // a *class* is no longer on the list on its own, and that is the flip: `Foo $x = null;` leaves this
        // unbound and is reported against the destination, naming `Foo?`
        //
        // asked of AST::bind_null_to, which is the same call AST::CallResolver makes for an argument this
        // position cannot reach: a direct call's parameter types sit on a declaration nobody has chosen
        // yet, so `expected_type` is null there and the binding happens once the callee is known
        if (expected_type != nullptr) {
            AST::bind_null_to(&node, expected_type->type);
        }
        cursor.skip();
        return AST::make_ref(node);
    }

    else if (cursor.is_type(Token::Type::t_string_literal)) {
        auto token = cursor.current();
        auto &node = payload.context.emplace_node<AST::LiteralStringExprNode>(token);

        // decoded here, at the one place a string literal is built, because reporting a bad escape needs
        // the collector and the node has none. the node keeps the *bytes* from this moment on; the token
        // stays verbatim for the code excerpt a diagnostic prints
        if (auto error = AST::decode_string_literal(token.value(), node.decoded_value)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(token), error->message);
        }

        // the type the literal *is*. stamped rather than looked up later, because result_type() has no
        // way to reach the collector - see the field's comment
        if (payload.collector.core_types.has(AST::CoreTypeKind::t_string)) {
            node.core_string_type = payload.collector.core_types.string_type();
        }

        cursor.skip();
        return AST::make_ref(node);
    }

    else if (cursor.is_type(Token::Type::t_string_interp_begin)) {
        auto *node = parse_string_interpolation(payload);

        if (node == nullptr) {
            return AST::NodeReference();
        }

        return AST::make_ref(*node);
    }

    // `mv E` - take the value out of E. one parse site covers every position a move can appear in
    // (`$b = mv $a`, `consume(mv $a)`, `return mv $a`), because all three arrive here through
    // parse_expr
    //
    // the operand is parsed by recursing into this function rather than by requiring a variable, so
    // `mv $doc->body` reaches AST::OwnershipPass as a real tree and gets the "partial moves are not
    // supported" diagnostic there - where the type is known - instead of an unexpected-token here
    else if (cursor.is_type(Token::Type::t_mv)) {
        auto move_token = cursor.current();
        cursor.skip();

        // the expected type flows through unchanged: `mv` says nothing about what E is, only about
        // who owns it afterwards
        auto operand_ref = parse_expr_node(payload, expected_type);

        if (!operand_ref.has() || !operand_ref.is_expression_node()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(move_token),
                "'mv' expects a value to move out of");
            return AST::make_void_ref();
        }

        auto *operand = operand_ref.unsafe_ptr<AST::ExprNode>();

        // only a place can be moved *out of*: there has to be storage left behind for the move to
        // empty. a non-place is already a move - a call result is nobody else's - so writing `mv` on
        // one is not an error to work around but a sign the author expected a copy to be happening
        if (!AST::is_place_expression(*operand)) {
            payload.collector.collect_issue<AST::Issue::MoveOfTemporary>(
                payload.context.code_ref(move_token),
                "'mv' needs an expression with storage to move out of - this value is already a temporary, so it moves on its own");
            return AST::make_void_ref();
        }

        auto &move_expr = payload.context.emplace_node<AST::MoveExprNode>(operand, move_token);
        return AST::make_ref(move_expr);
    }

    else if (cursor.is_type(Token::Type::t_ref)
        && cursor.peek_is_type(1, Token::Type::t_identifier))
    {
        return parse_function_ref(payload, expected_type);
    }

    else if (
        cursor.is_type(Token::Type::t_varname) ||
        cursor.is_type_sequence(0, { Token::Type::t_ref, Token::Type::t_varname })
    ) {
        // if the token is a reference operator we have to handle it
        bool is_creating_ptr = cursor.is_type(Token::Type::t_ref);
        if (is_creating_ptr) {
            cursor.skip();
        }

        auto var_token = cursor.current();
        auto found = payload.context.scope().lookup_variable(var_token.value());

        if (!found.decl) {
            payload.collector.collect_issue<AST::Issue::UnknownVariable>(payload.context.code_ref(cursor.current()), cursor.current().value());
            cursor.skip();
            return AST::make_void_ref();
        }

        // the name resolves, but to storage in a frame this one cannot reach. inside a closure that is a
        // *capture*: the value is copied into the closure's environment at the creation site, and the body
        // reads the copy. anywhere else - a plain nested `function`, which has no environment - it is an
        // error, because lowering it would load from an alloca belonging to a different llvm::Function and
        // CodegenContext::var_map would hand over a perfectly valid one
        AST::ExprNode *captured_read = nullptr;

        if (found.crossed_function_boundary()) {
            if (payload.context.current_closure_ptr == nullptr) {
                // a file-scope declaration is one frame out like any other - its storage is a local of
                // the implicit entry point - but saying "an enclosing function" about it names a
                // function the source does not contain, so it gets its own phrasing
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(var_token),
                    found.declared_at_file_scope
                        ? fmt::format(
                            "'{}' is declared at file scope, which is the implicit entry point's own "
                            "frame - a function body cannot reach it. Pass it as a parameter, or write "
                            "a closure.",
                            var_token.value())
                        : fmt::format(
                            "'{}' is declared in an enclosing function. A plain nested function captures "
                            "nothing - pass it as a parameter, or write a closure.",
                            var_token.value()));
                cursor.skip();
                return AST::make_void_ref();
            }

            captured_read =
                Parser::capture_variable(payload, found.decl, var_token, found.boundaries_crossed);

            if (captured_read == nullptr) {
                cursor.skip();
                return AST::make_void_ref();
            }
        }

        auto vardecl = found.decl;

        cursor.skip(); // skip the variable name

        // the base is either the variable itself, or the read of the environment's copy of it
        AST::NodeReference current_ref = AST::make_void_ref();

        if (captured_read != nullptr) {
            current_ref = AST::make_ref(captured_read);
        }
        else {
            auto &varnode = payload.context.emplace_node<AST::VarNode>(vardecl, var_token);
            current_ref = AST::make_ref(payload.context.emplace_node<AST::VarRefNode>(&varnode));
        }

        // wrap the base in a MemberAccessNode for each `->member` in the chain
        current_ref = Parser::parse_postfix_chain(payload, current_ref);
        if (!current_ref.has()) {
            return AST::make_void_ref();
        }

        // `$f(...)` - a call through a callable *value*. only a `(` directly after the chain can mean
        // this: a place expression is never followed by one otherwise, so there is nothing to
        // disambiguate against and no need to snapshot
        if (!is_creating_ptr && cursor.is_type(Token::Type::t_open_paren)) {
            auto *callee = current_ref.unsafe_ptr<AST::ExprNode>();

            if (auto *call = Parser::parse_indirect_call(payload, callee, var_token)) {
                return Parser::parse_postfix_chain(payload, AST::make_ref(*call));
            }

            return AST::make_void_ref();
        }

        if (is_creating_ptr) {
            // `&` applies to whatever the postfix chain produced, so `&$s->field` works and is
            // not an assert: taking the address of a VarRefNode that has already been replaced
            // with a MemberAccessNode is the wrong node to ask about
            auto *target = current_ref.unsafe_ptr<AST::ExprNode>();
            if (!AST::is_place_expression(*target)) {
                payload.collector.collect_issue<AST::Issue::AddressOfTemporary>(
                    payload.context.code_ref(var_token),
                    "Cannot take the address of an expression that has no storage");
                return AST::make_void_ref();
            }

            // `&$a[]` borrows the slot an append just grew, to be filled field by field - it does
            // not read what is in it. see AST::IndexExprNode::slot_is_bound, whose other setter is
            // the assignment target in Parser::parse_varexpr
            if (target->get_node_type() == AST::NodeType::n_expr_index) {
                static_cast<AST::IndexExprNode *>(target)->slot_is_bound = true;
            }

            // **the destination decides.** a `weak<T>` slot, parameter, field or return type is asking for
            // a weak reference, and this `&` is how it is spelled; anything else is asking for an address
            // and gets one, whatever the operand's type happens to be. see AddrOfExprNode's header for why
            // it cannot be keyed on the operand instead - the stdlib's generic accessors are the reason
            const bool weak_wanted = expected_type != nullptr && expected_type->type.is_weak();

            auto &ptr_expr = payload.context.emplace_node<AST::AddrOfExprNode>(target, weak_wanted);
            return AST::make_ref(ptr_expr);
        }

        return current_ref;
    }

    // a closure literal, `function(int32 $a) : int32 { ... }`. a value, so it belongs here rather than
    // in the statement dispatch - `starts_funcdecl` is what keeps a *declaration* out of this arm
    else if (Parser::starts_closure_literal(cursor)) {
        auto *closure = Parser::parse_closure_literal(payload);

        if (closure == nullptr) {
            return AST::make_void_ref();
        }

        return AST::make_ref(*closure);
    }

    // `weak($obj)` and `weak<Foo>($obj)` - the written spelling of what `&$obj` on a class already means.
    // both forms build exactly the node the `&` arm builds, so there is one lowering and one ownership
    // rule for the operation however it was written
    //
    // the explicit type argument is accepted and *checked*, not used: the target is whatever the operand
    // already is, so `weak<Bar>($aFoo)` is a mistake worth naming rather than a coercion. it exists so a
    // reader can say what they mean at a site where the operand's type is not obvious
    //
    // unlike `&`, a non-class operand is an error here rather than falling back to a borrow. `&` has to
    // stay total - it is the address-of operator for every type - but nobody writes `weak(...)` meaning
    // "take an address"
    else if (
        cursor.is_type_sequence(0, { Token::Type::t_weak, Token::Type::t_open_paren }) ||
        cursor.is_type_sequence(0, { Token::Type::t_weak, Token::Type::t_open_angle })
    ) {
        return AST::make_ref(Parser::parse_weak_expr(payload));
    }

    // `strong($w)` - the upgrade back, and the only way to read through a weak reference
    else if (cursor.is_type_sequence(0, { Token::Type::t_strong, Token::Type::t_open_paren })) {
        return AST::make_ref(Parser::parse_strong_expr(payload));
    }

    // `const(E)` - a demand rather than a hint: AST::ConstFolding replaces it with the literal it folded
    // to, and refuses with a location when it could not. written like `weak(...)` and `strong(...)` and
    // parsed through the same parenthesised-operand reader, so the three keyword-call forms have one shape
    else if (Parser::starts_const_expr(cursor)) {
        auto const_token = cursor.current();

        cursor.skip(); // `const`

        // **the destination reaches through the marker.** a `const(...)` becomes the literal it folds to,
        // and the type that literal has to have is the destination's - so the operand is parsed with the
        // same hint it would have been given had the marker not been written. without it `255 + 1` at a
        // `uint8` destination folds as int32 256 and the assignment narrows it to 0, silently, which is
        // precisely the answer AST::const_fold's overflow refusal exists to prevent
        auto *operand = parse_parenthesized_operand(payload, expected_type);

        if (operand == nullptr) {
            return AST::make_void_ref();
        }

        return AST::make_ref(&payload.context.emplace_node<AST::ConstExprNode>(const_token, operand));
    }

    // an explicit pointer cast, `ptr<uint8>($ints:$)` or `int32&($p:$)`. it reinterprets an
    // address as pointing at a different type, so its argument is almost always a `:$`
    // expression
    //
    // `ptr` always starts one; a plain identifier only does when a `&` and a `(` follow, which
    // no other production spells - `Foo(...)` is a constructor call and `Foo &$x` a declaration
    else if (
        cursor.is_type_sequence(0, { Token::Type::t_ptr, Token::Type::t_open_angle }) ||
        cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_ref, Token::Type::t_open_paren }) ||
        cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_and, Token::Type::t_open_paren })
    ) {
        auto cast_token = cursor.current();

        auto *cast_type = Parser::parse_type(payload);
        if (cast_type == nullptr) {
            return AST::make_void_ref();
        }

        if (!cast_type->type.is_pointer()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(cast_token),
                "Only a pointer type can be written as a cast");
            return AST::make_void_ref();
        }

        auto *inner = parse_parenthesized_operand(payload, cast_type);
        if (inner == nullptr) {
            return AST::make_void_ref();
        }

        auto &cast = payload.context.emplace_node<AST::TypeCastNode>(cast_type->type, inner, false);
        return Parser::parse_postfix_chain(payload, AST::make_ref(cast));
    }

    // `self::MAX` - a constant of the type this expression is written inside.
    //
    // a soft keyword, the precedent being `constructor` and `type` in a struct body: `self` is an ordinary
    // identifier to the lexer, and it is recognised here by value. It has to be claimed *before* the
    // namespace walk below, because NamespaceManager::retrieve creates what it does not find - a namespace
    // literally called `self` would be minted and then never resolve to anything.
    //
    // which type it means is not decided here: AST::FunctionBodyScope deliberately carries no self type, so
    // a method body has no way back to its owner at parse time. The expander answers it from the enclosing
    // declaration instead
    if (cursor.is_type(Token::Type::t_identifier) && cursor.current().value() == "self"
        && cursor.peek_is_type(1, Token::Type::t_namespace_sep)) {
        const auto self_token = cursor.current();
        cursor.skip(2); // `self` and the `::`

        if (!cursor.is_type(Token::Type::t_identifier)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(self_token),
                "'self::' names a constant of the type it is written inside - write `self::NAME`.");
            cursor.try_skip_to_next_statement();
            return AST::make_void_ref();
        }

        auto &const_ref = payload.context.emplace_node<AST::ConstRefExprNode>(
            cursor.current(), payload.context.current_namespace, /*is_qualified=*/true);
        const_ref.is_self_qualified = true;
        cursor.skip();

        return Parser::parse_postfix_chain(payload, AST::make_ref(const_ref));
    }

    // **a static call, `Type::f(...)`, and a static property, `Type::$x`** - both claimed before the
    // namespace walk below for the reason the `self::` arm above gives: parse_namespace mints what it
    // does not find, so by the time it has consumed `Point::` there is a namespace called `Point` and
    // the type it named is out of reach. a `$name` after the `::` would then fall off the end of this
    // function entirely, since neither the call arm nor the constant arm accepts one
    if (auto *static_call = Parser::try_parse_static_call(payload)) {
        return Parser::parse_postfix_chain(payload, AST::make_ref(*static_call));
    }

    if (auto *static_property = Parser::try_parse_static_property(payload)) {
        return Parser::parse_postfix_chain(payload, AST::make_ref(*static_property));
    }

    // there might be a namespace used
    // like
    //   std::math::sin(1.0)
    //   std::math::PI
    //   std::math::$foo
    const AST::Namespace *ast_namespace = nullptr;
    if (cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_namespace_sep })) {
        auto ns_node = parse_namespace(payload);
        assert(ns_node != nullptr && "expected a namespace node");
        ast_namespace = ns_node->ast_namespace;
    }

    // potential function call - `name(...)` or, with explicit type arguments, `name<...>(...)`
    //
    // `name<` is only a call if a `(` follows the type argument list. a bare identifier can be a
    // comparison operand - a compile-time constant is one - so `LIMIT < $n` would otherwise be
    // read as a call to `LIMIT<$n>`. parse_funccall speculates for us and hands the tokens back
    // untouched when it declines
    if (
        cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_open_paren }) ||
        cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_open_angle })
    ) {
        bool is_call = false;
        auto fcall = parse_funccall(payload, ast_namespace, &is_call);

        if (fcall != nullptr) {
            // **and then the suffixes**, which every other operand producer in this function already does.
            // without it a `->` after a free call was `Unexpected token '->'` - not a decision about
            // reading a member off a call result, just the one arm that forgot to continue the chain
            return Parser::parse_postfix_chain(payload, AST::make_ref(*fcall));
        }

        // committed to a call and it did not parse - the diagnostic is already collected
        if (is_call) {
            return AST::make_void_ref();
        }

        // not a call after all - fall through and read the identifier as an operand in its own right
    }

    // a bare identifier that is not a call names a **compile-time constant**: `MAX`, `std::math::PI`,
    // `buffer::MAX`. It is the one operand shape whose meaning cannot be settled here - a constant's name is
    // published by the declaration pass, which runs over every file, and a use site written above the
    // declaration or in another module is the ordinary case rather than the exception. So the node records
    // the name and where to look for it, and AST::ConstantExpander replaces it with a clone of the
    // constant's value - or reports an unknown constant, at this token
    if (cursor.is_type(Token::Type::t_identifier)) {
        auto &const_ref = payload.context.emplace_node<AST::ConstRefExprNode>(
            cursor.current(),
            ast_namespace != nullptr ? ast_namespace : payload.context.current_namespace,
            /*is_qualified=*/ast_namespace != nullptr);

        cursor.skip();

        return Parser::parse_postfix_chain(payload, AST::make_ref(const_ref));
    }

    // `&` reached here means it was not followed by a variable name, so there is no storage to
    // take the address of - `&5` and `&get()` are the two ways to spell that. reported with the
    // same message the place check further up uses, rather than falling into the catch-all
    // (`&5` and `&get()` have no storage)
    if (cursor.is_type(Token::Type::t_ref) || cursor.is_type(Token::Type::t_and)) {
        payload.collector.collect_issue<AST::Issue::AddressOfTemporary>(
            payload.context.code_ref(cursor.current()),
            "Cannot take the address of an expression that has no storage");
        cursor.try_skip_to_next_statement();
        return AST::make_void_ref();
    }

    // nothing in the grammar starts an operand with this token. an assert here aborted the whole
    // compiler with no location and no message the user could act on - and in a release build,
    // where the assert is compiled out, it fell off the end of a function that has to return
    payload.collect_unexpected_token(Token::Type::t_varname);
    cursor.try_skip_to_next_statement();
    return AST::make_void_ref();
}

// parse an operand appearing in prefix position, consuming any leading unary
// '-' / '+' operators and wrapping negations in a UnaryExprNode. unary '+' is
// a no-op and returns the operand unchanged
const AST::NodeReference parse_prefix_unary(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;

    // the prefix position: the built-in `-` / `+`, and any symbol a user declared prefix. consumed
    // here rather than in the shunting yard, which has no notion of a one-operand operator - and that
    // placement is also what gives the chapter's "the precedence of unary operators is higher than
    // that of binary operators" for free, since the symbol and its operand are already one operand by
    // the time the yard sees them
    const auto match = operator_match_at(payload, cursor);

    const bool builtin_prefix = match.has()
        && (match.op->type == Token::Type::t_op_sub || match.op->type == Token::Type::t_op_add
            || match.op->is_prefix_only());
    const bool declared_prefix = match.has() && match.op->has_fixity(AST::OpFixity::t_prefix);

    if (builtin_prefix || declared_prefix) {
        auto op_token = cursor.current();
        cursor.skip(match.token_count);

        // recurse so chained prefixes like `- -$x` and `!!-$x` resolve right to left
        auto operand = parse_prefix_unary(payload, expected_type);
        if (!operand.has()) {
            return AST::make_void_ref();
        }

        auto *operand_expr = operand.unsafe_ptr<AST::ExprNode>();

        // **which meaning applies is decided here, not at the symbol** - it takes the operand's type,
        // and that is not known until the operand has been parsed. so a declared `operator -(Point)`
        // does not stop `-$x` over an int32 from being an ordinary negation
        if (declared_prefix
            && !AST::unary_has_builtin_meaning(match.op, AST::parse_time_operand(operand_expr))) {

            auto *call = Parser::build_operator_call(
                payload, *match.op, AST::OpFixity::t_prefix, op_token, {operand_expr});

            if (call == nullptr) {
                return AST::make_void_ref();
            }

            return Parser::parse_postfix_chain(payload, AST::make_ref(*call));
        }

        // unary plus carries no semantics
        if (match.op->type == Token::Type::t_op_add) {
            return operand;
        }

        auto &unary = payload.context.emplace_node<AST::UnaryExprNode>(op_token, operand_expr);
        return AST::make_ref(unary);
    }

    // a parenthesized subexpression, e.g. -(a + b)
    if (cursor.is_type(Token::Type::t_open_paren)) {
        cursor.skip();
        auto inner = Parser::parse_expr_ref(payload, expected_type);
        if (cursor.is_type(Token::Type::t_close_paren)) {
            cursor.skip();
        }
        return inner;
    }

    // an operand, and then whatever suffix operators follow it. this and the yard's own operand arm
    // are the two operand positions in the grammar, so wrapping both is what makes `1m` and
    // `$distance mm` the same rule rather than a literal special case
    return parse_suffix_operator_chain(payload, parse_expr_node(payload, expected_type));
}

// an aggregate, so it is appended with `push_back({...})` rather than `emplace_back(a, b)`: the
// two-argument emplace needs C++20 parenthesized aggregate initialization (P0960), which AppleClang
// does not implement. the braced temporary costs a copy of two words - do not "fix" it back
struct ExprPart
{
    // val node
    const AST::NodeReference node;
    // operator node
    AST::OperatorNode *opnode;
};

#define O1Prec part.opnode->op->precedence
#define O2Prec operator_stack.top()->op->precedence

std::vector<ExprPart> shunting_yard(const std::vector<ExprPart> &expr_parts)
{
    auto output = std::vector<ExprPart>();
    auto operator_stack = std::stack<AST::OperatorNode *>();

    for (auto part : expr_parts) {
        // if its a literal, variable etc. (not an operator)
        if (part.opnode == nullptr) {
            output.push_back(part);
        }
        else if (part.opnode->op->type == Token::Type::t_open_paren) {
            operator_stack.push(part.opnode);
        }
        else if (part.opnode->op->type == Token::Type::t_close_paren) {
            while (!operator_stack.empty() && operator_stack.top()->op->type != Token::Type::t_open_paren) {
                output.push_back({AST::make_void_ref(), operator_stack.top()});
                operator_stack.pop();
            }

            // ensure we have the opening "(", otherwise something is off
            assert(operator_stack.top()->op->type == Token::Type::t_open_paren);
            operator_stack.pop();
        }
        else {
            while (
                !operator_stack.empty() &&
                // O2Prec.assoc != AST::OpAssociativity::left &&
                operator_stack.top()->op->type != Token::Type::t_open_paren &&
                (
                    O2Prec.sequence < O1Prec.sequence ||
                    (
                        O1Prec.sequence == O2Prec.sequence &&
                        O1Prec.assoc == AST::OpAssociativity::left
                    )
                )
            ) {
                output.push_back({AST::make_void_ref(), operator_stack.top()});
                operator_stack.pop();
            }

            operator_stack.push(part.opnode);
        }
    }

    while (!operator_stack.empty()) {
        output.push_back({AST::make_void_ref(), operator_stack.top()});
        operator_stack.pop();
    }

    return output;
}

const AST::NodeReference parse_expr_parts(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;

    // **`guard` is not an expression, and is deliberately not an `is_expr_token` either.** so the loop
    // below never enters and the single-part reporter at the end would say "this expression could not
    // be read as a single value", which describes the shape and not the mistake. reported here instead,
    // ahead of everything, because this is the one function every value position routes through
    //
    // it is not an expression on purpose: a guard's else arm may hold a bare `return` precisely because
    // AST::scope_always_exits refuses an arm that rejoins, so the arm can never produce a value. an
    // expression form would make that `return` read two ways at once
    if (cursor.is_type(Token::Type::t_guard)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(cursor.current()),
            "'guard' is not an expression - it is how a declaration's initializer is written, so it "
            "may only follow the '=' of a declaration. Write 'T $x = guard <value> else { ... }' above "
            "this statement and read '$x' here.");
        cursor.try_skip_to_next_statement();
        return AST::make_void_ref();
    }

    std::vector<ExprPart> expr_parts;

    int depth = 0;

    auto token = cursor.here();
    auto tvalue = token.value();

    while (is_expr_token(payload, cursor)) {
        // if we have a closing parenthesis and the depth is 0, we can break the loop
        // because we have reached the end of the expression
        if (cursor.is_type(Token::Type::t_close_paren) && depth == 0) {
            break;
        }

        // try to parse an operator. a *sequence* lookup, not a single-token one: a declared symbol may
        // be spelled out of several ordinary tokens (`!!`, `<=>`), and the lexer knows nothing about
        // any of them
        const auto match = operator_match_at(payload, cursor);
        const AST::Operator *op = match.op;

        // a '-' or '+' in operand position is a prefix unary operator, not a
        // binary one. detect it and parse the operand it applies to, otherwise
        // the shunting yard would hand parse_binary_expr a null lhs and crash
        bool expects_operand = expr_parts.empty() ||
            (expr_parts.back().opnode != nullptr &&
             expr_parts.back().opnode->op->type != Token::Type::t_close_paren);

        // ...and the same is true of any symbol declared in *prefix* position. without this arm a word
        // operator in operand position - the plain call `avg(1.0, 2.0)`, where `avg` is also declared
        // infix - would be read as an operator with nothing on its left
        if (op != nullptr && expects_operand &&
            (op->type == Token::Type::t_op_sub || op->type == Token::Type::t_op_add
                || op->is_prefix_only() || op->has_fixity(AST::OpFixity::t_prefix)))
        {
            auto node = parse_prefix_unary(payload, expected_type);
            if (!node.has()) {
                return AST::make_void_ref();
            }
            expr_parts.push_back({node, nullptr});
            continue;
        }

        // **an infix operator, or nothing.** a *custom* symbol declared only in prefix or suffix
        // position is not one, and has to fall through so it is parsed as part of an operand instead:
        // `500mm` reaches here after an operand, and reading a suffix-only `mm` as infix would make
        // the yard pop two operands for it
        //
        // a built-in symbol is always usable here whatever anybody declared, because its infix meaning
        // is the language's - `$a - $b` does not stop being a subtraction because somebody wrote a
        // prefix `-` for a struct
        // ...and a custom symbol in **operand** position is not an infix operator either, whatever it
        // was declared as. that is what keeps a word operator usable as a function name: with `avg`
        // declared infix, the ordinary call `avg(1.0, 2.0)` arrives here at offset 0, where reading it
        // as an operator leaves the yard popping two operands for something with no left operand at
        // all. falling through parses it as the operand it is
        //
        // the one built-in exception is `!`, which has no infix meaning to be the language's - it is
        // asked here only after the prefix arm above declined, so what is left is `$a ! $b`, and
        // falling through reports it as two expressions with nothing between them
        const bool usable_here = op != nullptr && !op->is_prefix_only()
            && (!op->is_custom() || (op->has_fixity(AST::OpFixity::t_infix) && !expects_operand));

        if (usable_here) {
            auto &opnode = payload.context.emplace_node<AST::OperatorNode>(cursor.current(), op);

            cursor.skip(match.token_count);
            expr_parts.push_back({AST::make_void_ref(), &opnode});

            // if the operator is a open parenthesis, we increase the depth
            if (op->type == Token::Type::t_open_paren) {
                depth++;
            } else if (op->type == Token::Type::t_close_paren) {
                depth--;
            }

            continue;
        }

        // **two operands with nothing joining them**, `echo 1 2`. caught here, where the cursor still
        // says where the second one starts. falling through to a `node_stack.size() == 1`
        // assert at the bottom of this function would take the compiler down instead of reporting
        //
        // the array literal production widened the ways to arrive here, because a `[` that no postfix
        // chain claimed now parses as one operand rather than being an unexpected token - `5[0]` and
        // `[1, 2][0]` reach it, and they are the whole of what is left. a *call* runs the chain, whose
        // bracket arm now indexes it: a call result can be given storage, and only a
        // genuinely addressless base is still refused there. so this needs to be a diagnostic before it
        // is anything else
        if (!expr_parts.empty() && expr_parts.back().opnode == nullptr) {
            const bool looks_like_indexing = cursor.is_type(Token::Type::t_open_bracket);

            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(cursor.current()),
                looks_like_indexing
                    ? "only a place can be indexed - a variable, a field or an element. Bind this "
                      "expression to a name first, then index that."
                    : fmt::format(
                        "unexpected '{}' - two expressions with no operator between them.",
                        cursor.current().value()));

            return AST::make_void_ref();
        }

        // parse the next expression node, plus any suffix operators applied to it
        auto node = parse_suffix_operator_chain(payload, parse_expr_node(payload, expected_type));

        // if the node is empty
        if (!node.has()) {
            return AST::make_void_ref();
        }

        expr_parts.push_back({node, nullptr});
    }

    // if we have only one part, we can return it directly
    if (expr_parts.size() == 1) {
        // **an operator with nothing to apply**, which the loop above reaches whenever it stops on a
        // token `is_expr_token` declines: `if (` claims the paren as an operator part and the loop
        // then breaks, leaving it alone. an assert here would abort the compiler naming this
        // function - which is how a missing production reads to whoever hit it, `!` having been one
        if (expr_parts[0].opnode != nullptr) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(expr_parts[0].opnode->token_literal),
                fmt::format("'{}' has no operand - an expression was expected after it.",
                    expr_parts[0].opnode->op->spelling));

            return AST::make_void_ref();
        }

        return expr_parts[0].node;
    }

    auto postfix_expr = shunting_yard(expr_parts);

    // build expressions nodes
    std::stack<AST::NodeReference> node_stack;
    for (auto &part : postfix_expr) {
        if (part.opnode != nullptr) {
            // a binary operator needs two operands. writing one where a value belongs -
            // `&($a + $b)`, or a stray leading `*` - would pop an empty stack and take the
            // compiler down with it, so report it as the syntax error it is
            if (node_stack.size() < 2) {
                payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                    payload.context.code_ref(part.opnode->token_literal),
                    Token::Type::t_unknown,
                    part.opnode->token_literal.type()
                );
                return AST::make_void_ref();
            }

            auto right = node_stack.top();
            node_stack.pop();

            auto left = node_stack.top();
            node_stack.pop();

            // let our binary expresssion parser take over
            // there is not much to parse here but it will handle type casts
            // and other node transformation to ensure echos expression behavior
            auto combined = parse_binary_expr(payload, part.opnode, left, right);

            // it can fail now that a declared operator is resolved in there, and a void ref pushed
            // back onto this stack would be the *next* operator's null left operand - dereferenced
            // one iteration later, with nothing left to say where it came from
            if (!combined.has()) {
                return AST::make_void_ref();
            }

            node_stack.push(combined);
        }
        else {
            node_stack.push(part.node);
        }
    }

    // the operand-after-operand check in the collection loop above is what guarantees this, so a
    // mismatch here is a parser bug rather than a bad program. reported rather than asserted all the
    // same: an assert takes the compiler down with no location at all, which is the worst of the
    // three possible outcomes even when the cause really is ours
    if (node_stack.size() != 1) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(token),
            "this expression could not be read as a single value.");
        return AST::make_void_ref();
    }

    return node_stack.top();
}

const AST::NodeReference Parser::parse_expr_ref(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    const AST::NodeReference expr = parse_expr_parts(payload, expected_type);

    // **the destination, applied once to the finished expression.**
    //
    // every operand above was parsed with the same hint, because a shunting yard cannot know which
    // operator is coming - so a hint that describes the expression's *result* rather than its operands
    // must not be handed down at all. AST::can_type_a_literal is that filter, and `bool` is the case it
    // exists for: without this step `bool $x = 3 < 4;` retyped both operands and compared two bools,
    // and with the filter alone `bool $a = 3;` would have been coerced in silence via icmp ne 0.
    // `bool $a = 1;` is typed here, as `true`; `3` is refused.
    //
    // costs nothing for the destinations the operands *were* given: those literals already carry a
    // chosen type, so AST::is_untyped_literal answers false and this does not fire
    if (expected_type == nullptr || !expr.has() || !AST::is_untyped_literal(expr.unsafe_ptr<AST::ExprNode>())) {
        return expr;
    }

    return apply_literal_typing(payload, expr.unsafe_ptr<AST::ExprNode>(), expected_type->type);
}
