#include "Parser/ReturnParser.h"

#include "Parser/ExprParser.h"

#include "AST/TypeNode.h"

AST::ReturnNode &Parser::parse_return(Parser::Payload &payload)
{
    // sanity check that the current token is a return keyword
    if (!payload.cursor.is_type(Token::Type::t_return)) {
        payload.collect_unexpected_token(Token::Type::t_return);
        payload.cursor.try_skip_to_next_statement();
        auto &expr = payload.context.emplace_node<AST::VoidExprNode>();

        return payload.context.emplace_node<AST::ReturnNode>(&expr);
    }

    auto return_token = payload.cursor.current();

    // skip the return keyword
    payload.cursor.skip();

    // a bare `return;` hands back nothing. it has to be recognised before the expression parse
    // rather than after: parse_expr on an empty expression tripped its own
    // `node_stack.size() == 1` assertion, so an early return from a void function crashed the
    // compiler instead of compiling to a `ret void`
    if (payload.cursor.is_type(Token::Type::t_semicolon)) {
        payload.cursor.skip();
        return payload.context.emplace_node<AST::ReturnNode>(nullptr, return_token);
    }

    // parse the expression that follows the return keyword, typed against the declared return
    // type the same way a variable declaration's initializer is typed against its variable
    //
    // only a concrete primitive is a useful hint though - the literal parsers apply the same rule
    // to themselves, this one keeps the hint from reaching the rest of the expression too
    AST::TypeNode *expected_type = payload.context.return_type_ptr;
    if (expected_type != nullptr && !AST::can_type_a_literal(expected_type->type)) {
        expected_type = nullptr;
    }

    auto expr = parse_expr(payload, expected_type);

    // ensure we have a semicolon at the end of the return statement
    if (!payload.cursor.is_type(Token::Type::t_semicolon)) {
        payload.collect_unexpected_token(Token::Type::t_semicolon);
        payload.cursor.try_skip_to_next_statement();
    }
    else {
        payload.cursor.skip();
    }

    return payload.context.emplace_node<AST::ReturnNode>(expr, return_token);
}