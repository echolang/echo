#include "Parser/VarDeclParser.h"

#include "AST/VarDeclNode.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/AssignNode.h"
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
    // the `&` suffix is part of the type grammar now, so parse_type returns the borrow already
    // built and there is nothing to patch up here
    if (can_parse_type(payload))  {
        type = parse_type(payload);
        if (type == nullptr) {
            cursor.try_skip_to_next_statement();
            return nullptr;
        }
        is_const = type->type.is_const();
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
        // const is *not* checked here. it used to be, on the declared type's top level, which is
        // the wrong level twice over: `const int& $r` is a mutable borrow of a const pointee, so
        // the guard never fired on the write it should reject, while `const ptr<int> $p` is a const
        // pointer whose pointee may legally be written, so it fired on a write it should allow.
        // telling those apart needs the deref AST::PointerAdjuster inserts, so the check now lives
        // in AST::TypeChecker::check_const_target, keyed on the assignment target's shape

        // we do not allow to redefine the type of a variable, the type
        // has to be either explictly set in the firt declaration or inferred
        if (!prev_vardecl->has_type() && type != nullptr) {
            payload.collector.collect_issue<AST::Issue::VariableRedeclaration>(payload.context.code_ref(nametoken), prev_vardecl);
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        // an assignment to an existing variable. the left hand side is a place expression:
        // the variable itself, or any `->member` chain hanging off it. both shapes produce the
        // same AssignNode, so codegen resolves them through one lvalue path
        auto var_node = &payload.context.emplace_node<AST::VarNode>(prev_vardecl);
        auto var_ref = &payload.context.emplace_node<AST::VarRefNode>(var_node);
        auto target_ref = Parser::parse_postfix_chain(payload, AST::make_ref(*var_ref));
        if (!target_ref.has()) {
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        auto *target = target_ref.unsafe_ptr<AST::ExprNode>();

        if (!payload.cursor.is_type(Token::Type::t_assign)) {
            payload.collect_unexpected_token(Token::Type::t_assign);
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        auto assign_token = cursor.current();
        cursor.skip();

        // the value is expected at the type the *storage* holds. for a pointer target that is
        // the pointee, because assigning to a pointer writes through it - `$p = 20` never
        // changes where $p points (book/concept/pointers_and_refs_v2.md, "Binding, writing,
        // and re-seating"). a declaration is the other case and binds instead, which is why
        // the init_expr path below keeps the full declared type
        auto &expected = payload.context.emplace_node<AST::TypeNode>(
            AST::value_result_type(*target));

        auto expr = parse_expr(payload, &expected);

        auto assign = &payload.context.emplace_node<AST::AssignNode>(target, expr, assign_token);

        // skip the end of the statement
        if (is_vardecl_end_token(cursor)) {
            if (should_skip_vardecl_end_token(cursor)) {
                cursor.skip();
            }
        }

        payload.context.scope().children.push_back(AST::make_ref(assign));

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
        payload.collect_unexpected_token(Token::Type::t_assign);
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
            // the inferred type is the single source of truth, const included - there is no
            // longer a separate node-level flag that could disagree with it.
            // value_result_type, not result_type: `$copy = $r` over an `int32&` copies the int
            // it refers to, so the copy is an int32 rather than a second reference
            auto inferred = AST::value_result_type(*vardecl->init_expr);
            vardecl->set_type_node(&payload.context.emplace_node<AST::TypeNode>(
                is_const ? AST::ValueType::make_const(inferred) : inferred));
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