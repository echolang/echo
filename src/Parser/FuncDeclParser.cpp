#include "Parser/FuncDeclParser.h"

#include "AST/VarDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/FunctionDeclNode.h"
#include "Parser/TypeParser.h"
#include "Parser/ExprParser.h"
#include "Parser/VarDeclParser.h"
#include "Parser/ScopeParser.h"

AST::FunctionDeclNode * Parser::parse_funcdecl(Parser::Payload &payload, bool symbol_only)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type(Token::Type::t_function)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_function, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the function keyword
    cursor.skip();

    // next token should be an identifier aka the function name
    if (!cursor.is_type(Token::Type::t_identifier)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_identifier, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // fetch the function name and skip it
    auto nametoken = cursor.current();
    cursor.skip();

    // next token needs to be an open parenthesis
    if (!cursor.is_type(Token::Type::t_open_paren)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_open_paren, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto fncsymbol = payload.collector.namespaces.find_symbol(nametoken.value(), *payload.context.current_namespace);
    AST::FunctionDeclNode *funcdecl = nullptr;

    if (fncsymbol != nullptr) {
        auto symboldecl = fncsymbol->node.get_ptr<AST::FunctionDeclNode>();
        if (symboldecl != nullptr) {
            funcdecl = symboldecl;

            // if the function is already defined, we reset the arguments
            // and reparse them with the new context
            funcdecl->args.clear();
        }
    }

    if (funcdecl == nullptr) {
        funcdecl = &payload.context.emplace_node<AST::FunctionDeclNode>(nametoken);
    }

    // set the namespace of the function
    funcdecl->ast_namespace = payload.context.current_namespace;

    // skip the open parenthesis
    cursor.skip();

    // create an empty base scope for the function and the arguments to sit in
    auto &funcscope = payload.context.emplace_node<AST::ScopeNode>();

    // parse the function arguments
    while (!cursor.is_type(Token::Type::t_close_paren)) {
        if (cursor.is_done()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(nametoken), Token::Type::t_close_paren, Token::Type::t_unknown);
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        auto vardecl = parse_vardecl(payload, &funcscope);
        funcdecl->args.push_back(vardecl);
    }

    // skip the close parenthesis
    cursor.skip();

    // next token should be ":" for the return type
    if (!cursor.is_type(Token::Type::t_colon)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_colon, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the colon
    cursor.skip();

    // parse the return type
    if (!can_parse_type(payload)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_identifier, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    funcdecl->return_type = parse_type(payload);

    // if we are only interested in the symbol, we are done
    if (symbol_only) {
        return funcdecl;
    }

    // we already add the function declaration to the scope
    // in case the function is recursive
    payload.context.scope().add_funcdecl(*funcdecl);

    // if next token is a semicolon we are done for now
    if (cursor.is_type(Token::Type::t_semicolon)) {
        cursor.skip();
        return funcdecl;
    }

    // if the next token is an open brace, we parse the function body
    if (!cursor.is_type(Token::Type::t_open_brace)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_open_brace, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the open brace
    cursor.skip();

    // push the function scope
    payload.context.push_scope(funcscope);

    funcdecl->body = &parse_scope(payload);

    // we expect a closing brace
    if (!cursor.is_type(Token::Type::t_close_brace)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_close_brace, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the closing brace
    cursor.skip();

    // pop the function scope
    payload.context.pop_scope();

    // payload.context.scope().children.push_back(AST::make_ref(funcdecl));
    return funcdecl;
}