#include "Parser/ScopeParser.h"

#include "AST/VarDeclNode.h"

#include "Parser/VarDeclParser.h"

AST::ScopeNode & Parser::parse_scope(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;
    auto &context = payload.context;

    auto &scope_node = context.emplace_node<AST::ScopeNode>();

    while (!cursor.is_done())
    {
        // var declaration 
        // can be:
        //   int $foo =
        //   $bar = 
        if (
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_assign }) ||
            cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_varname, Token::Type::t_assign })
        ) {
            auto &vardecl = parse_vardecl(payload);
            scope_node.children.push_back(AST::make_ref(vardecl));
        }

        else {
            // payload.context.
        }
    }

    return scope_node;
}
