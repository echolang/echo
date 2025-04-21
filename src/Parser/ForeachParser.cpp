#include "Parser/ForeachParser.h"

#include "Parser/ExprParser.h"
#include "Parser/ScopeParser.h"

#include <fmt/core.h>

AST::ForeachNode *Parser::parse_foreach(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type(Token::Type::t_foreach)) {
        payload.collect_unexpected_token(Token::Type::t_foreach);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto &node = payload.context.emplace_node<AST::ForeachNode>(cursor.current());
    cursor.skip();

    auto give_up = [&]() -> AST::ForeachNode * {
        cursor.try_skip_to_next_statement();
        return nullptr;
    };

    // **the paren is consumed here, not handed to parse_expr.** is_expr_token admits both parens
    // unconditionally, so passing it the opening one - the way parse_whilestatement can, because its
    // condition runs to the brace - leaves the shunting yard holding an unbalanced group when the
    // expression stops at `as`
    if (!cursor.is_type(Token::Type::t_open_paren)) {
        payload.collect_unexpected_token(Token::Type::t_open_paren);
        return give_up();
    }
    cursor.skip();

    node.source = parse_expr(payload);

    if (node.source == nullptr) {
        return give_up();
    }

    if (!cursor.is_type(Token::Type::t_as)) {
        payload.collect_unexpected_token(Token::Type::t_as);
        return give_up();
    }

    cursor.skip();

    // the keyed form, `$k => $el`. decided by lookahead rather than by parsing a binding and finding a
    // `=>` after it, so the binding-mode grammar below is read exactly once
    if (cursor.is_type(Token::Type::t_varname) && cursor.peek_is_type(1, Token::Type::t_double_arrow)) {
        const auto key_token = cursor.current();
        cursor.skip();

        node.token_arrow.emplace(cursor.current());
        cursor.skip();

        // untyped: AST::ForeachLowering fills in the type node once the key type is known. a key is
        // always by value, so no binding mode is read for it
        node.key = &payload.context.emplace_node<AST::VarDeclNode>(key_token, nullptr);
    }

    // the binding mode, in this order and only this order: `const` then `&`
    bool wrote_const = false;

    if (cursor.is_type(Token::Type::t_const)) {
        node.token_binding.emplace(cursor.current());
        wrote_const = true;
        cursor.skip();
    }

    // `&$el` lexes as t_ref and `& $el` as t_and - LexerFunction::ReferenceFrom wins only when the `&`
    // abuts an identifier character. both spell the same thing, exactly as Parser::parse_ref_suffix
    // accepts both on a type
    const bool wrote_ref = cursor.is_type(Token::Type::t_ref) || cursor.is_type(Token::Type::t_and);

    if (wrote_ref) {
        if (!node.token_binding.has_value()) {
            node.token_binding.emplace(cursor.current());
        }
        cursor.skip();
    }

    // `&const $el` - spelled the other way round. refused rather than accepted, because the two orders
    // reading alike is what makes one of them a typo nobody notices
    if (cursor.is_type(Token::Type::t_const)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(cursor.current()),
            "write 'const &$el' - the 'const' describes the element, so it goes before the '&'.");
        return give_up();
    }

    if (wrote_const && !wrote_ref) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(node.token_binding.value()),
            "'const' on a by-value binding promises nothing - the copy is already yours to write. "
            "write 'const &$el' to borrow the element read-only, or drop the 'const'.");
        return give_up();
    }

    if (wrote_ref) {
        node.binding = wrote_const
            ? AST::ForeachNode::Binding::t_const_borrow
            : AST::ForeachNode::Binding::t_borrow;
    }

    if (!cursor.is_type(Token::Type::t_varname)) {
        payload.collect_unexpected_token(Token::Type::t_varname);
        return give_up();
    }

    node.element = &payload.context.emplace_node<AST::VarDeclNode>(cursor.current(), nullptr);
    cursor.skip();

    if (!cursor.is_type(Token::Type::t_close_paren)) {
        payload.collect_unexpected_token(Token::Type::t_close_paren);
        return give_up();
    }
    cursor.skip();

    if (!cursor.is_type(Token::Type::t_open_brace)) {
        payload.collect_unexpected_token(Token::Type::t_open_brace);
        return give_up();
    }

    const auto loop_brace = cursor.current();
    cursor.skip();

    {
        // a loop body like any other, so `break` and `continue` work in it - and they work *correctly*
        // without a step block, because the lowering puts the iterator's advance in the condition
        AST::LoopScope loop(payload.context, payload.context.loop_depth + 1);

        // the two bindings are seeded, so `$el` and `$k` resolve while the statements that read them are
        // parsed. that is also what lets AST::ForeachLowering fill in their types later rather than
        // splice declarations: they are already body->children[0..1]
        std::vector<AST::VarDeclNode *> seeds;
        if (node.key != nullptr) {
            seeds.push_back(node.key);
        }
        seeds.push_back(node.element);

        node.body = &parse_scope(payload, loop_brace, std::move(seeds));
    }

    if (!cursor.is_type(Token::Type::t_close_brace)) {
        payload.collect_unexpected_token(Token::Type::t_close_brace);
        return give_up();
    }
    cursor.skip();

    return &node;
}
