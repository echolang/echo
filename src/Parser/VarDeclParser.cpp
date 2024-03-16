#include "Parser/VarDeclParser.h"

#include "AST/VarDeclNode.h"
#include "Parser/TypeParser.h"

AST::VarDeclNode & Parser::parse_vardecl(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    auto &vardecl = payload.context.emplace_node<AST::VarDeclNode>();
        type, cursor.peek()
    );

    // when we have an identifier we assume it to be the variable type
    if (can_parse_type(payload))
    {
        auto &type = parse_type(payload);
        auto &vardecl = payload.context.module.nodes.emplace_back<AST::VarDeclNode>(
            type, cursor.peek()
        );

        cursor.skip_until(Token::Type::t_semicolon);

        return vardecl;
    }
    parse_type(payload);

    if (token.type() == Token::Type::t_varname)
    {
        auto &vardecl = context.module.nodes.emplace_back<AST::VarDeclNode>(
            token, token
        );

        cursor.skip_until(Token::Type::t_semicolon);

        scope_node.children.push_back(AST::make_ref<AST::VarDeclNode>(&vardecl));
    }

    cursor.skip();
}