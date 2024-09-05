#include "Parser/ScopeParser.h"

#include "AST/VarDeclNode.h"
#include "AST/ExprNode.h"

#include "Parser/VarDeclParser.h"
#include "Parser/EchoPrintParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/FuncCallParser.h"
#include "Parser/IfStatementParser.h"
#include "Parser/ReturnParser.h"
#include "Parser/WhileStatementParser.h"
#include "Parser/NamespaceParser.h"
#include "Parser/AttributeParser.h"
#include "Parser/StructParser.h"
#include "Parser/TypeParser.h"

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
            context.scope().add_child_scope(parse_scope(payload));

            // next token needs to be a closing brace
            if (!cursor.is_type(Token::Type::t_close_brace))
            {
                payload.collect_unexpected_token(Token::Type::t_close_brace);
                cursor.try_skip_to_next_statement();
                break;
            }

            cursor.skip();
        }
        else if (cursor.is_type(Token::Type::t_close_brace))
        {
            break;
        }
        else if (cursor.is_type(Token::Type::t_namespace))
        {
            parse_namespacedecl(payload);
        }
        else if (cursor.is_type(Token::Type::t_function))
        {
            parse_funcdecl(payload);
        }
        else if (cursor.is_type(Token::Type::t_struct))
        {
            parse_struct(payload);
        }
        else if (cursor.is_type(Token::Type::t_return))
        {
            scope_node.children.push_back(AST::make_ref(parse_return(payload)));
        }
        else if (cursor.is_type(Token::Type::t_if))
        {
            scope_node.children.push_back(AST::make_ref(parse_ifstatement(payload)));
        }
        else if (cursor.is_type(Token::Type::t_while))
        {
            scope_node.children.push_back(AST::make_ref(parse_whilestatement(payload)));
        }
        // print statement aka "echo $something"
        else if (cursor.is_type(Token::Type::t_echo)) {
            if (auto *echo_node = parse_echo(payload)) { 
                scope_node.children.push_back(AST::make_ref(echo_node));
            }
        }
        // attribute definition
        //   #[attr]
        //   myfunc() {...
        else if (cursor.is_type(Token::Type::t_hash))
        {
            parse_attribute(payload);
        }

        // var declaration 
        // can be:
        //   int $foo =
        //   $bar = 
        //   const $ey
        else if (
            cursor.is_type(Token::Type::t_const) || // const keyword always starts a vardecl
            cursor.is_type(Token::Type::t_ptr) || // ptr keyword also indicates a vardecl
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_assign }) ||
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_accessorlr }) ||
            // `$p:$ = ...` re-seats a pointer, so the statement's left hand side is a place
            // expression rather than a bare name
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_ptr_of }) ||
            cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_varname, Token::Type::t_assign }) ||
            cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_varname, Token::Type::t_semicolon }) ||
            starts_borrow_vardecl(payload) || // int32& $r = ..., either `&` spelling
            starts_qualified_vardecl(payload) // a::b::Foo $foo
        ) {
            parse_varexpr(payload, &scope_node);
        }

        else if (cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_open_paren })) {
            if (auto *funccall_node = parse_funccall(payload)) {
                scope_node.children.push_back(AST::make_ref(funccall_node));
                
                // expect a semicolon after function call statement
                if (!cursor.is_type(Token::Type::t_semicolon)) {
                    payload.collect_unexpected_token(Token::Type::t_semicolon);
                    cursor.try_skip_to_next_statement();
                } else {
                    cursor.skip(); // consume the semicolon
                }
            }
        }

        else {
            payload.collect_unexpected_token(Token::Type::t_unknown);

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