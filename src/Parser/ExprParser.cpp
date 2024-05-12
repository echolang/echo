#include "Parser/ExprParser.h"

AST::ExprNode &Parser::parse_expr(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    if (cursor.is_type(Token::Type::t_floating_literal)) {
        auto &node = payload.context.emplace_node<AST::LiteralFloatExprNode>(cursor.current());
        cursor.skip();
        return node;
    }

    if (cursor.is_type(Token::Type::t_integer_literal)) {
        auto &node = payload.context.emplace_node<AST::LiteralIntExprNode>(cursor.current());
        cursor.skip();
        return node;
    }

    if (cursor.is_type(Token::Type::t_bool_literal)) {
        auto &node = payload.context.emplace_node<AST::LiteralBoolExprNode>(cursor.current());
        cursor.skip();
        return node;
    }

    assert(false && "unimplemented");
}