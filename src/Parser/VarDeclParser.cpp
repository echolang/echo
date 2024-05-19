#include "Parser/VarDeclParser.h"

#include "AST/VarDeclNode.h"
#include "AST/TypeNode.h"
#include "Parser/TypeParser.h"
#include "Parser/ExprParser.h"

void Parser::parse_vardecl(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    AST::TypeNode *type = nullptr;
    AST::VarDeclNode *vardecl = nullptr;
    bool is_const = false;

    // when we have an identifier we assume it to be the variable type
    if (can_parse_type(payload))  {
        type = &parse_type(payload);
        is_const = type->is_const;
    }

    // special case is "const" but type must be inferred
    // const $ronon = 10;
    else if (cursor.is_type(Token::Type::t_const)) {
        cursor.skip();
        is_const = true;
    }

    // fetch the varname and skip it
    auto nametoken = cursor.current();
    cursor.skip();

    // ensure that we actually have a varname
    if (nametoken.type() != Token::Type::t_varname) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(nametoken), Token::Type::t_varname, nametoken.type());
        cursor.try_skip_to_next_statement();
        return;
    }

    // check if the name is already taken in the current scope
    auto prev_vardecl = payload.context.scope().find_vardecl_by_name(nametoken.value());

    // we have a previous declaration, this might be a mutable variable
    if (prev_vardecl != nullptr) 
    {    
        // if the variable is a const, we cannot redeclare nor mutate it
        if (!prev_vardecl->has_type() && prev_vardecl->type_node()->is_const) {
            payload.collector.collect_issue<AST::Issue::VariableRedeclaration>(payload.context.code_ref(nametoken), prev_vardecl);
            cursor.try_skip_to_next_statement();
            return;
        }

        // we do not allow to redefine the type of a variable, the type 
        // has to be either explictly set in the firt declaration or inferred
        if (!prev_vardecl->has_type() && type != nullptr) {
            payload.collector.collect_issue<AST::Issue::VariableRedeclaration>(payload.context.code_ref(nametoken), prev_vardecl);
            cursor.try_skip_to_next_statement();
            return;
        }
    }

    vardecl = &payload.context.emplace_node<AST::VarDeclNode>(nametoken, type);
    payload.context.scope().add_vardecl(*vardecl);

    // if next token is a semicolon we are done for now
    if (cursor.current().type() == Token::Type::t_semicolon) {
        cursor.skip();
        return;
    }

    if (!payload.cursor.is_type(Token::Type::t_assign)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_assign, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return;
    }

    cursor.skip();

    // parse the expression
    auto expr = parse_expr(payload, vardecl->optional_type_node());
    vardecl->init_expr = expr;

    if (!vardecl->has_type()) {
        // if there is no explicit type we need to be able to infer it
        if (vardecl->init_expr == nullptr) {
            payload.collector.collect_issue<AST::Issue::GenericError>(payload.context.code_ref(cursor.current()), "cannot infer type of variable without an initializer");
            cursor.try_skip_to_next_statement();
            return;
        }
        else {
            vardecl->set_type_node(&payload.context.emplace_node<AST::TypeNode>(vardecl->init_expr->result_type()));
            vardecl->type_node()->is_const = is_const;
        }
    }

    // skip the semicolon
    cursor.skip();
}