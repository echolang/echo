#include "Parser/ScopeParser.h"

#include "AST/VarDeclNode.h"

#include "Parser/VarDeclParser.h"

AST::ScopeNode & Parser::parse_scope(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;
    auto &context = payload.context;

    auto &scope_node = context.emplace_node<AST::ScopeNode>();

    context.push_scope(scope_node);

    while (!cursor.is_done())
    {
        // deep scope
        if (cursor.is_type(Token::Type::t_open_brace))
        {
            cursor.skip();
            parse_scope(payload);
        }
        else if (cursor.is_type(Token::Type::t_close_brace))
        {
            cursor.skip();
            break;
        }

        // var declaration 
        // can be:
        //   int $foo =
        //   $bar = 
        //   const $ey
        else if (
            cursor.is_type(Token::Type::t_const) || // const keyword always starts a vardecl
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_assign }) ||
            cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_varname, Token::Type::t_assign }) || 
            cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_varname, Token::Type::t_semicolon })
        ) {
            parse_vardecl(payload);
        }

        else {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(context.code_ref(cursor.current()), Token::Type::t_unknown, cursor.current().type());

            // when we encounter an unexpected token, we skip until we find a semicolon or a brace
            // in the hopes that there is    simply a typo in the code or something minor that we can recover from
            // we might have to skip till the end of the scope otherwise..
            cursor.skip(); // always skip the token causing the issue
            cursor.skip_until({ Token::Type::t_semicolon, Token::Type::t_open_brace, Token::Type::t_close_brace });

            // if we find a semicolon or a close brace, we skip it in the hopes that afterwards we can continue parsing
            if (cursor.is_type({ Token::Type::t_semicolon, Token::Type::t_close_brace })) {
                cursor.skip();
            }
        }
    }

    context.pop_scope();

    return scope_node;
}
