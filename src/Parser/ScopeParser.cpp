#include "Parser/ScopeParser.h"

#include "AST/VarDeclNode.h"

AST::ScopeNode & Parser::parse_scope(Cursor &cursor, AST::Context &context)
{
    auto &scope_node = context.module.nodes.emplace_back<AST::ScopeNode>();

    while (!cursor.is_done())
    {
        auto token = cursor.current();

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

    return scope_node;
}
