#include "Parser/FuncCallParser.h"
#include "Parser/ExprParser.h"
#include "Parser/TypeParser.h"

#include "AST/FunctionDeclNode.h"
#include "AST/VarDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/ASTArgumentCoercion.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/OperatorNode.h"
#include "AST/TypeCastNode.h"
#include <map>
#include <functional>
#include <unordered_map>

AST::FunctionCallExprNode *Parser::parse_funccall(Parser::Payload &payload, const AST::Namespace *requested_namespace)
{
    // a call is `name(` or, with explicit type arguments, `name<...>(`
    if (!payload.cursor.is_type_sequence(0, {Token::Type::t_identifier, Token::Type::t_open_paren}) &&
        !payload.cursor.is_type_sequence(0, {Token::Type::t_identifier, Token::Type::t_open_angle})) {
        payload.collect_unexpected_token(Token::Type::t_identifier);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto funcname_token = payload.cursor.current();

    // skip the function name
    payload.cursor.skip();

    // optional explicit type arguments: name<int, float>(...)
    std::vector<AST::TypeNode *> explicit_type_args;
    if (payload.cursor.is_type(Token::Type::t_open_angle)) {
        payload.cursor.skip(); // skip '<'

        while (!payload.cursor.is_generic_close()) {
            if (payload.cursor.is_done()) {
                payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(funcname_token), Token::Type::t_close_angle, Token::Type::t_unknown);
                payload.cursor.try_skip_to_next_statement();
                return nullptr;
            }

            auto *type_node = parse_type(payload);
            if (type_node) {
                explicit_type_args.push_back(type_node);
            }

            if (payload.cursor.is_type(Token::Type::t_comma)) {
                payload.cursor.skip();
            }
        }

        payload.cursor.consume_generic_close(); // consume '>' (splitting a '>>' if present)
    }

    // the open parenthesis is required
    if (!payload.cursor.is_type(Token::Type::t_open_paren)) {
        payload.collect_unexpected_token(Token::Type::t_open_paren);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the open parenthesis
    payload.cursor.skip();

    // parse the arguments
    std::vector<AST::ExprNode *> args;
    while (!payload.cursor.is_type(Token::Type::t_close_paren)) {
        if (payload.cursor.is_done()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(funcname_token), Token::Type::t_close_paren, Token::Type::t_unknown);
            payload.cursor.try_skip_to_next_statement();
            return nullptr;
        }

        auto arg = parse_expr(payload);
        if (arg == nullptr) {
            // an issue was already collected by the failed sub-parse; abort this call
            // rather than propagating a null argument into the funccall node
            payload.cursor.try_skip_to_next_statement();
            return nullptr;
        }
        args.push_back(arg);

        if (payload.cursor.is_type(Token::Type::t_comma)) {
            payload.cursor.skip();
        }
    }

    // skip the close parenthesis
    payload.cursor.skip();

    auto &funcall = payload.context.emplace_node<AST::FunctionCallExprNode>(funcname_token, args);
    funcall.explicit_type_args = explicit_type_args;

    // try to find the function declaration
    funcall.decl = payload.context.scope().find_funcdecl_by_name(funcname_token.value());

    // if no function declaration was found, try to locate an external symbol
    if (funcall.decl == nullptr) {
        
        // if a namespace was provided, try to find the symbol in that namespace
        if (!requested_namespace) {
            requested_namespace = payload.context.current_namespace;
        }

        auto symbol = payload.collector.namespaces.find_symbol(funcname_token.value(), *requested_namespace);
        if (symbol) {
            funcall.decl = symbol->node.get_ptr<AST::FunctionDeclNode>();
        }
    }

    // if no function declaration was found, we have an issue
    if (funcall.decl == nullptr) {
        payload.collector.collect_issue<AST::Issue::UnknownFunction>(payload.context.code_ref(funcname_token), funcname_token.value());
        return nullptr;
    }

    // generic calls keep pointing at the template here; the monomorphizer resolves the
    // concrete instance and rewrites funcall.decl (and inserts casts) after parsing.
    // coerce arguments only for non-generic decls, whose parameter types are concrete
    if (!funcall.decl->is_generic()) {
        for (size_t i = 0; i < args.size() && i < funcall.decl->args.size(); ++i) {
            auto expected = funcall.decl->args[i]->type();
            // a variable passed to a pointer parameter is coerced to its address here, so
            // codegen sees a uniform AddrOfExprNode instead of sniffing the argument's kind
            auto *expr = AST::coerce_arg_to_pointer_param(payload.context.module.nodes, args[i], expected);
            auto actual = expr->result_type();

            auto coerce_expr = [&](AST::ExprNode *source, const AST::ValueType &from, const AST::ValueType &to) -> AST::ExprNode * {
                // is_implicitly_convertible rather than ==, so a borrow passed where a nullable
                // pointer is expected does not acquire a cast codegen has no lowering for
                if (AST::is_implicitly_convertible(from, to)) {
                    return source;
                }

                // Insert an implicit cast node for any remaining mismatches
                auto &cast = payload.context.emplace_node<AST::TypeCastNode>(to, source, true);
                return &cast;
            };

            auto *coerced = coerce_expr(expr, actual, expected);
            args[i] = coerced;
            funcall.arguments[i] = coerced;
        }
    }
    
    return &funcall;
}
