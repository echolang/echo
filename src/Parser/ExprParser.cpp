#include "Parser/ExprParser.h"

#include "AST/ExprNode.h"
#include "AST/VarRefNode.h"
#include "AST/LiteralValueNode.h"

#include "External/infint.h"

#include <format>

bool can_hold_literal_int(Parser::Payload &payload, AST::ValueType type, const std::string &literal, const TokenReference literal_token)
{
    InfInt value(literal);

    auto int_size = AST::get_integer_size(type.get_primitive_type());

    if (value > int_size.get_max_positive_value()) {
        payload.collector.collect_issue<AST::Issue::IntegerOverflow>(
            payload.context.code_ref(literal_token), 
            std::format(
                "The literal '{}' is too large for the integer type '{}'. The maximum value is '{}'.", 
                literal,
                AST::get_primitive_name(type.get_primitive_type()),
                int_size.get_max_positive_value()
            )
        );

        return false;
    }

    if (value < int_size.get_max_negative_value()) {
        payload.collector.collect_issue<AST::Issue::IntegerUnderflow>(
            payload.context.code_ref(literal_token), 
            std::format(
                "The literal '{}' is too small for the integer type '{}'. The minimum value is '{}'.", 
                literal,
                AST::get_primitive_name(type.get_primitive_type()),
                int_size.get_max_negative_value()
            )
        );

        return false;
    }

    return true;
}

const AST::NodeReference parse_literal_float(Parser::Payload &payload, AST::TypeNode *expected_type)
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
            // even if the number doesn't fit into the expected type, we can continue because the value is still valid
            // we just loose precision and the user gets a warning
            auto &casted_node = payload.context.emplace_node<AST::LiteralFloatExprNode>(current_token, expected_type->type.get_primitive_type());

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

                    // override the literal value with the float value
                    casted_node.override_literal_value.emplace(std::to_string(fliteral));
                }
            }

            return AST::make_ref(casted_node);
        }

        // integers
        else if (expected_type->type.is_integer()) 
        {
            // determine if the literal has any decimal values besides 0
            // if so, we emit a error (not just a warning) because the user highly likely made a mistake
            // or is expecting a wrong type.
            double dliteral = std::stod(node.get_fvalue_string());
            double dliteral_cmp = (double) (long long) dliteral;

            if (dliteral != dliteral_cmp) {
                payload.collector.collect_issue<AST::Issue::InvalidTypeConversion>(
                    payload.context.code_ref(current_token), 
                    std::format(
                        "The floating point number literal '{}' cannot be implicitly converted to an integer type due to non zero decimal values.", 
                        node.get_fvalue_string()
                    )
                );

                return AST::make_void_ref();
            }

            // if we end up here our floating point number is a whole number
            // so we can safely convert it to an integer, but we still have to check 
            // if the integer type will fit the literal 

            // the int literal is simply the fvalue string with everything after the dot removed
            std::string int_literal = node.get_fvalue_string().substr(0, node.get_fvalue_string().find('.'));

            if (!can_hold_literal_int(payload, expected_type->type, int_literal, current_token)) {
                return AST::make_void_ref();
            }

            auto &casted_node = payload.context.emplace_node<AST::LiteralIntExprNode>(current_token, expected_type->type.get_primitive_type());
            casted_node.override_literal_value.emplace(int_literal);

            return AST::make_ref(casted_node);
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
    return AST::make_ref(node);
}


const AST::NodeReference parse_literal_int(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;
    auto &node = payload.context.emplace_node<AST::LiteralIntExprNode>(cursor.current());

    // if there is a specified expected type, check if the literal fits the type
    // if (expected_type != nullptr) 

    cursor.skip();

    return AST::make_ref(node);
}

AST::ExprNode *Parser::parse_expr(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto ref = parse_expr_ref(payload, expected_type);

    // probably a bad idea, but it should never be not a expr node
    return ref.unsafe_ptr<AST::ExprNode>();
}

const AST::NodeReference Parser::parse_expr_ref(Parser::Payload &payload, AST::TypeNode *expected_type)
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
        return AST::make_ref(node);
    }

    if (cursor.is_type(Token::Type::t_varname)) {
        auto vardecl = payload.context.scope().find_vardecl_by_name(cursor.current().value());

        if (!vardecl) {
            payload.collector.collect_issue<AST::Issue::UnknownVariable>(payload.context.code_ref(cursor.current()), cursor.current().value());
            cursor.skip();
            return AST::make_void_ref();
        }   

        auto &varref = payload.context.emplace_node<AST::VarRefNode>(cursor.current(), vardecl);
        auto &node = payload.context.emplace_node<AST::VarRefExprNode>(&varref);
        cursor.skip();
        
        return AST::make_ref(node);
    }

    assert(false && "unimplemented");
}