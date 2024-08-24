#include "Parser/VarDeclParser.h"

#include "AST/VarDeclNode.h"
#include "AST/VarMutNode.h"
#include "AST/MemberMutNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/TypeNode.h"
#include "Parser/TypeParser.h"
#include "Parser/ExprParser.h"

bool is_vardecl_end_token(const Parser::Cursor &cursor)
{
    return cursor.is_type(Token::Type::t_semicolon) || cursor.is_type(Token::Type::t_comma) || cursor.is_type(Token::Type::t_close_paren);
}

// we do not want to actually skip a closing parenthesis
// because the parent parse will check it to ensure it has parsed all arguments
bool should_skip_vardecl_end_token(const Parser::Cursor &cursor)
{
    return cursor.is_type(Token::Type::t_semicolon) || cursor.is_type(Token::Type::t_comma);
}

AST::VarDeclNode *Parser::parse_varexpr(Parser::Payload &payload, AST::ScopeNode *scope)
{
    auto &cursor = payload.cursor;

    AST::TypeNode *type = nullptr;
    AST::VarDeclNode *vardecl = nullptr;
    bool is_const = false;

    // when we have an identifier we assume it to be the variable type
    if (can_parse_type(payload))  {
        type = parse_type(payload);
        is_const = type->is_const;
        
        // Check for reference modifier after the type
        if (cursor.is_type(Token::Type::t_ref)) {
            cursor.skip(); // skip the '&'
            
            // Create a new ValueType with the pointer flag set
            auto updated_value_type = type->type;
            updated_value_type.set_pointer(true);
            
            // Create a new TypeNode with the updated ValueType
            if (type->type_token.has_value()) {
                type = &payload.context.emplace_node<AST::TypeNode>(updated_value_type, type->type_token.value());
            } else {
                type = &payload.context.emplace_node<AST::TypeNode>(updated_value_type);
            }
            type->is_const = is_const;
            type->is_pointer = true;
        }
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
        return nullptr;
    }

    // check if the name is already taken in the current scope
    AST::VarDeclNode *prev_vardecl = nullptr;
    if (scope != nullptr) {
        prev_vardecl = scope->find_vardecl_by_name(nametoken.value());
    }

    // if the next token is a accessor this is a member reference

    // we have a previous declaration, this might be a mutable variable
    if (prev_vardecl != nullptr) 
    {    
        // if the variable is a const, we cannot redeclare nor mutate it
        if (!prev_vardecl->has_type() && prev_vardecl->type_node()->is_const) {
            payload.collector.collect_issue<AST::Issue::VariableRedeclaration>(payload.context.code_ref(nametoken), prev_vardecl);
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        // we do not allow to redefine the type of a variable, the type 
        // has to be either explictly set in the firt declaration or inferred
        if (!prev_vardecl->has_type() && type != nullptr) {
            payload.collector.collect_issue<AST::Issue::VariableRedeclaration>(payload.context.code_ref(nametoken), prev_vardecl);
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        // Check if this is a member access mutation ($var->member = value)
        if (payload.cursor.is_type(Token::Type::t_accessorlr)) {
            // Parse the member access pattern
            auto var_node = &payload.context.emplace_node<AST::VarNode>(prev_vardecl);
            auto var_ref = &payload.context.emplace_node<AST::VarRefNode>(var_node);
            auto current_ref = AST::make_ref(*var_ref);

            // wrap the base in a MemberAccessNode for each `->member` in the chain.
            // this block is only entered on a leading `->`, so at least one level
            // runs and the final node is always a MemberAccessNode
            current_ref = Parser::parse_member_chain(payload, current_ref);
            if (!current_ref.has()) {
                cursor.try_skip_to_next_statement();
                return nullptr;
            }
            auto member_access = current_ref.get_ptr<AST::MemberAccessNode>();

            // Now expect the assignment operator
            if (!payload.cursor.is_type(Token::Type::t_assign)) {
                payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_assign, cursor.current().type());
                cursor.try_skip_to_next_statement();
                return nullptr;
            }
            
            cursor.skip(); // skip the '=' token
            
            // Parse the value expression
            auto expr = parse_expr(payload, nullptr); // We'll infer the type from the member
            
            // Create the member mutation node
            auto member_mut = &payload.context.emplace_node<AST::MemberMutNode>(member_access, expr);
            
            // Skip the end of the statement
            if (is_vardecl_end_token(cursor)) {
                if (should_skip_vardecl_end_token(cursor)) {
                    cursor.skip();
                }
            }
            
            // Add the member mutation to the scope
            payload.context.scope().children.push_back(AST::make_ref(member_mut));
            
            return nullptr; // Don't return a VarDeclNode since this is a mutation
        }
        
        // Regular variable assignment ($var = value)
        if (!payload.cursor.is_type(Token::Type::t_assign)) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_assign, cursor.current().type());
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        cursor.skip();

        // parse the expression
        auto expr = parse_expr(payload, prev_vardecl->type_node());

        // generate a var mutation node
        auto varmut = &payload.context.emplace_node<AST::VarMutNode>(nametoken, expr, prev_vardecl);

        // skip the end of the statement
        if (is_vardecl_end_token(cursor)) {
            if (should_skip_vardecl_end_token(cursor)) {
                cursor.skip();
            }
        }

        // add the varmut to the scope
        payload.context.scope().children.push_back(AST::make_ref(varmut));

        return nullptr;
    }

    vardecl = &payload.context.emplace_node<AST::VarDeclNode>(nametoken, type);

    // if we have a scope we add the variable to it
    if (scope != nullptr) {
        scope->add_vardecl(*vardecl);
    }

    // if next token is a semicolon or comma we are done for now
    if (is_vardecl_end_token(cursor)) {
        if (should_skip_vardecl_end_token(cursor)) {
            cursor.skip();
        }
        return vardecl;
    }

    if (!payload.cursor.is_type(Token::Type::t_assign)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_assign, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return nullptr;
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
            return nullptr;
        }
        else {
            vardecl->set_type_node(&payload.context.emplace_node<AST::TypeNode>(vardecl->init_expr->result_type()));
            vardecl->type_node()->is_const = is_const;
            // For inferred pointer types, don't set is_pointer = true since the ValueType already has the pointer flag
            vardecl->type_node()->is_pointer = false;
        }
    }

    // skip the end of the statement
    if (is_vardecl_end_token(cursor)) {
        if (should_skip_vardecl_end_token(cursor)) {
            cursor.skip();
        }
    }

    return vardecl;
}