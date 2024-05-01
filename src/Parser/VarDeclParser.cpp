#include "Parser/VarDeclParser.h"

#include "AST/VarDeclNode.h"
#include "Parser/TypeParser.h"

AST::VarDeclNode & Parser::parse_vardecl(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    AST::TypeNode *type = nullptr;
    AST::VarDeclNode *vardecl = nullptr;

    // when we have an identifier we assume it to be the variable type
    if (can_parse_type(payload))  {
        type = &parse_type(payload);    
    }

    vardecl = &payload.context.emplace_node<AST::VarDeclNode>(cursor.current(), type);

    
    
    if (type == nullptr) {
        // if there is no explicit type we need to be able to infer it

    }

    // if (token.type() == Token::Type::t_varname)
    // {
    //     auto &vardecl = context.module.nodes.emplace_back<AST::VarDeclNode>(
    //         token, token
    //     );

    //     cursor.skip_until(Token::Type::t_semicolon);

    //     scope_node.children.push_back(AST::make_ref<AST::VarDeclNode>(&vardecl));
    // }

    cursor.skip();

    return *vardecl;
}