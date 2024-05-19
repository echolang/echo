#include "Parser/ExprParser.h"

#include "AST/ExprNode.h"
#include "AST/VarRefNode.h"

#include <format>

AST::LiteralIntExprNode *parse_literal_int(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;
    auto &node = payload.context.emplace_node<AST::LiteralIntExprNode>(cursor.current());

    // todo: store the required subtype in the AST node
    // ensure that the literal is parsed correctly and an error is emitted if 
    // the literal doesnt fit the expected type like 600 for a byte

    cursor.skip();

    return &node;
}


AST::LiteralFloatExprNode *parse_literal_float(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;

    auto current_token = cursor.current();
    auto &node = payload.context.emplace_node<AST::LiteralFloatExprNode>(current_token);
    cursor.skip();

    // if there is a specified expected type, check if the literal fits the type
    if (expected_type != nullptr) 
    {
        // floats / doubles
        if (expected_type->type.is_floating_type()) 
        {
            // if the actual type is a float32 and the expected type is a float64, emit an warning
            if (node.result_type().will_fit_into(expected_type->type) == false) {
                
                // we do a quick check if the literal would actually loose precision
                // I personally see no point in annyoing the user with a warning if the literal is 1.0
                // so if we can cast the double to float and back to double and the value is the same, we dont emit a warning
                double dliteral = std::stod(node.get_fvalue_string());
                float fliteral = (float) dliteral;
                double dliteral2 = (double) fliteral;

                if (dliteral != dliteral2) {
                    payload.collector.collect_issue<AST::Issue::LossOfPrecision>(
                        payload.context.code_ref(current_token), 
                        std::format(
                            "The literal '{}' is stored in 32bit float which will result in the effctive value {}", 
                            node.get_fvalue_string(),
                            fliteral
                        )
                    );
                }
            }

            // even if the number doesn't fit into the expected type, we can continue because the value is still valid
            // we just loose precision and the user gets a warning
            auto &casted_node = payload.context.emplace_node<AST::LiteralFloatExprNode>(current_token, expected_type->type.get_primitive_type());
            return &casted_node;
        }

        // integers
        else if (expected_type->type.is_numeric_type()) 
        {
            assert(false && "unimplemented");
        }
        else {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                payload.context.code_ref(current_token), 
                Token::Type::t_unknown,
                current_token.type()
            );
        }
    }

    // no expected type, just parse the literal
    return &node;
}

AST::ExprNode *Parser::parse_expr(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;

    if (cursor.is_type(Token::Type::t_floating_literal)) {
        return parse_literal_float(payload, expected_type);
    }

    if (cursor.is_type(Token::Type::t_integer_literal)) {
        return parse_literal_int(payload, expected_type);
    }

    if (cursor.is_type(Token::Type::t_bool_literal)) {
        auto &node = payload.context.emplace_node<AST::LiteralBoolExprNode>(cursor.current());
        cursor.skip();
        return &node;
    }

    if (cursor.is_type(Token::Type::t_varname)) {
        auto vardecl = payload.context.scope().find_vardecl_by_name(cursor.current().value());

        if (!vardecl) {
            payload.collector.collect_issue<AST::Issue::UnknownVariable>(payload.context.code_ref(cursor.current()), cursor.current().value());
            cursor.skip();
            return nullptr;
        }   

        auto &varref = payload.context.emplace_node<AST::VarRefNode>(cursor.current(), vardecl);
        auto &node = payload.context.emplace_node<AST::VarRefExprNode>(&varref);
        cursor.skip();
        
        return &node;
    }

    assert(false && "unimplemented");
}