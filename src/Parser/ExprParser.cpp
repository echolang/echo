#include "Parser/ExprParser.h"

#include "AST/ASTOps.h"
#include "AST/ExprNode.h"
#include "AST/VarRefNode.h"
#include "AST/VarNode.h"
#include "AST/OperatorNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/TypeCastNode.h"
#include "AST/MemberAccessNode.h"

#include "External/infint.h"

#include "Parser/FuncCallParser.h"
#include "Parser/NamespaceParser.h"

#include <format>
#include <stack>

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

std::string get_fstring_literal(std::string value)
{
    // remove trailing zeros
    value.erase(value.find_last_not_of('0') + 1, std::string::npos);
    // leave at least one digit after the dot
    if (value.back() == '.') {
        value += '0';
    }
    
    return value;
}

std::string get_f64_string_literal(double value)
{
    return get_fstring_literal(std::to_string(value));
}

std::string get_f32_string_literal(float value)
{
    return get_fstring_literal(std::to_string(value)) + "f";
}

/**
 * AUTOCAST FLOAT
 * ----------------------------------------------------------------------------
 * 
 * Autocast a "float" literal to the expected type.
 * 
 * Float literals can be implicitly converted to:
 *  - to larger float types (float32 to float64)
 *  - smaller float types (float64 to float32) can throw a warning if the value is not representable
 *  - to integer types (float to int) can throw an error if the value is not a whole number
 */
const AST::NodeReference autocast_literal_float(Parser::Payload &payload, AST::LiteralFloatExprNode &node, const AST::ValueType *expected_type)
{
    auto literal_token = node.token_literal;

    // if there is a specified expected type, check if the literal fits the type
    if (expected_type != nullptr) 
    {
        // floats / doubles
        if (expected_type->is_floating_type()) 
        {
            // even if the number doesn't fit into the expected type, we can continue because the value is still valid
            // we just loose precision and the user gets a warning
            auto &casted_node = payload.context.emplace_node<AST::LiteralFloatExprNode>(literal_token, expected_type->get_primitive_type());

            // if the actual type is a float32 and the expected type is a float64, emit an warning
            if (node.result_type().will_fit_into(*expected_type) == false) {
                
                // we do a quick check if the literal would actually loose precision
                // I personally see no point in annyoing the user with a warning if the literal is 1.0
                // so if we can cast the double to float and back to double and the value is the same, we dont emit a warning
                double dliteral = std::stod(node.get_fvalue_string());
                float fliteral = (float) dliteral;
                double dliteral2 = (double) fliteral;

                if (dliteral != dliteral2) {
                    payload.collector.collect_issue<AST::Issue::LossOfPrecision>(
                        payload.context.code_ref(literal_token), 
                        std::format(
                            "The literal '{}' is stored in 32bit float which will result in the effctive value {}", 
                            node.get_fvalue_string(),
                            fliteral
                        )
                    );

                    // override the literal value with the float value
                    casted_node.override_literal_value.emplace(get_f32_string_literal(fliteral));
                }
                else {
                    casted_node.override_literal_value.emplace(node.get_fvalue_string() + "f");
                }
            }

            // if the exptected type is a float64, we define the override literal value without the "f" suffix
            // at least if there is one in the first place
            if (
                expected_type->get_primitive_type() == AST::ValueTypePrimitive::t_float64 && 
                node.effective_token_literal_value().back() == 'f'
            ) {
                casted_node.override_literal_value.emplace(node.get_fvalue_string());
            }

            return AST::make_ref(casted_node);
        }

        // integers
        else if (expected_type->is_integer_type()) 
        {
            // determine if the literal has any decimal values besides 0
            // if so, we emit a error (not just a warning) because the user highly likely made a mistake
            // or is expecting a wrong type.
            double dliteral = std::stod(node.get_fvalue_string());
            double dliteral_cmp = (double) (long long) dliteral;

            if (dliteral != dliteral_cmp) {
                payload.collector.collect_issue<AST::Issue::InvalidTypeConversion>(
                    payload.context.code_ref(literal_token), 
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

            if (!can_hold_literal_int(payload, *expected_type, int_literal, literal_token)) {
                return AST::make_void_ref();
            }

            auto &casted_node = payload.context.emplace_node<AST::LiteralIntExprNode>(literal_token, expected_type->get_primitive_type());
            casted_node.override_literal_value.emplace(int_literal);

            return AST::make_ref(casted_node);
        }
        
        // cannot cast
        else {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                payload.context.code_ref(literal_token), 
                Token::Type::t_unknown,
                literal_token.type()
            );
        }
    }

    // otherwise just return the original node
    return AST::make_ref(node);
}

/**
 * AUTOCAST INT
 * ----------------------------------------------------------------------------
 * 
 * Autocast a "int" literal to the expected type.
 *
 * Integer literals can be implicitly converted to:
 *  - to "float" types (int to float) can always cause a loss of precision (TODO emit warning)
 *  - to larger integer types (int8 to int16) can always be done
 *  - to smaller integer types (int64 to int8) can throw an error if the value is not representable
 *  - to boolean types (int to bool) can always be done, 0 is false, everything else is true
 */
const AST::NodeReference autocast_literal_int(Parser::Payload &payload, AST::LiteralIntExprNode &node, const AST::ValueType *expected_type)
{
    auto literal_token = node.token_literal;
    InfInt intvalue(literal_token.value());

    if (expected_type != nullptr) 
    {
        // floats / doubles
        // if the expected type is a float, we can "safely" convert the integer to a float
        if (expected_type->is_floating_type()) 
        {
            // we can safely convert the integer to a float
            auto &casted_node = payload.context.emplace_node<AST::LiteralFloatExprNode>(literal_token, expected_type->get_primitive_type());

            if (expected_type->get_primitive_type() == AST::ValueTypePrimitive::t_float32) 
            {
                float val = casted_node.float_value();
                casted_node.override_literal_value.emplace(get_f32_string_literal(val));
            } 
            else if (expected_type->get_primitive_type() == AST::ValueTypePrimitive::t_float64) 
            {
                double val = casted_node.double_value();
                casted_node.override_literal_value.emplace(get_f64_string_literal(val));
            } 
            else {
                payload.collector.collect_issue<AST::Issue::InvalidTypeConversion>(
                    payload.context.code_ref(literal_token), 
                    std::format(
                        "The integer literal '{}' cannot be implicitly converted to the expected type '{}'.", 
                        literal_token.value(),
                        expected_type->get_type_desciption()
                    )
                );

                return AST::make_void_ref();
            }

            // @TODO we should add a detection if the float value is actually the same as the integer value
            // as very large integers will loose precision when converted to a float
            return AST::make_ref(casted_node);
        }

        // integers
        else if (expected_type->is_integer_type())
        {
            auto &expected_node = payload.context.emplace_node<AST::LiteralIntExprNode>(literal_token, expected_type->get_primitive_type());

            // check if the expected type is unsigned and the literal is negative
            // which should throw an error
            if (expected_type->is_unsigned_integer() && intvalue < 0) {
                payload.collector.collect_issue<AST::Issue::InvalidTypeConversion>(
                    payload.context.code_ref(literal_token), 
                    std::format(
                        "The integer literal '{}' cannot be implicitly converted to an unsigned integer because it is negative.", 
                        literal_token.value()
                    )
                );

                return AST::make_void_ref();
            }

            // check if the literal fits the expected type
            auto int_size = AST::get_integer_size(expected_type->get_primitive_type());
            auto lower_bound = int_size.get_max_negative_value();
            auto upper_bound = int_size.get_max_positive_value();

            if (intvalue < lower_bound) {
                payload.collector.collect_issue<AST::Issue::IntegerUnderflow>(
                    payload.context.code_ref(literal_token), 
                    std::format(
                        "The literal '{}' is too small for the integer type '{}'. The minimum value is '{}'.", 
                        literal_token.value(),
                        AST::get_primitive_name(expected_type->get_primitive_type()),
                        lower_bound
                    )
                );

                return AST::make_void_ref();
            }

            if (intvalue > upper_bound) {
                payload.collector.collect_issue<AST::Issue::IntegerOverflow>(
                    payload.context.code_ref(literal_token), 
                    std::format(
                        "The literal '{}' is too large for the integer type '{}'. The maximum value is '{}'.", 
                        literal_token.value(),
                        AST::get_primitive_name(expected_type->get_primitive_type()),
                        upper_bound
                    )
                );

                return AST::make_void_ref();
            }

            // if we end up here, the literal fits the expected type and can be used as expected
            return AST::make_ref(expected_node);
        }

        // booleans
        else if (expected_type->is_boolean_type()) 
        {
            // we can convert to a boolean, we consider 0 as false and everything else as true
            auto &casted_node = payload.context.emplace_node<AST::LiteralBoolExprNode>(literal_token);
            casted_node.override_literal_value.emplace(intvalue == 0 ? "false" : "true");
            return AST::make_ref(casted_node);
        }

        // cannot cast
        else {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                payload.context.code_ref(literal_token), 
                Token::Type::t_unknown,
                literal_token.type()
            );
        }
    }

    // otherwise just return the original node
    return AST::make_ref(node);
}


/**
 * FLOAT LITERAL
 * ----------------------------------------------------------------------------
 * 
 * Parse a float literal and return a node reference to it.
 */
const AST::NodeReference parse_literal_float(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;

    auto current_token = cursor.current();
    auto &node = payload.context.emplace_node<AST::LiteralFloatExprNode>(current_token);
    cursor.skip();

    auto expected_type_ptr = expected_type ? &expected_type->type : nullptr;

    // handle autocasting 
    return autocast_literal_float(payload, node, expected_type_ptr);
}

/**
 * INTEGER LITERAL
 * ----------------------------------------------------------------------------
 * 
 * Parse a integer literal and return a node reference to it.
 */
const AST::NodeReference parse_literal_int(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;
    auto current_token = cursor.current();

    // we first check if the literal is larger then a 32bit integer, if so we automatically create a 64bit integer
    InfInt intvalue(current_token.value());
    auto guessed_int_type = AST::ValueTypePrimitive::t_int32;

    if (intvalue > AST::get_integer_size(AST::ValueTypePrimitive::t_int32).get_max_positive_value()) {
        guessed_int_type = AST::ValueTypePrimitive::t_int64;
    }

    auto &node = payload.context.emplace_node<AST::LiteralIntExprNode>(current_token, guessed_int_type);
    cursor.skip();

    auto expected_type_ptr = expected_type ? &expected_type->type : nullptr;

    // handle autocasting
    return autocast_literal_int(payload, node, expected_type_ptr);
}

/**
 * HEX LITERAL
 * ----------------------------------------------------------------------------
 * 
 * Parse a hex literal and return a node reference to it.
 */
const AST::NodeReference parse_literal_hex(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;
    auto current_token = cursor.current();

    // we first check if the literal is larger then a 32bit integer, if so we automatically create a 64bit integer
    std::string hex_value = current_token.value();
    auto hex_len = hex_value.length() - 2; // remove the "0x" prefix

    // determine the integer type based on the string length of the hex value
    auto guessed_int_type = AST::ValueTypePrimitive::t_uint64;
    if (hex_len <= 2) {
        guessed_int_type = AST::ValueTypePrimitive::t_uint8;
    } else if (hex_len <= 4) {
        guessed_int_type = AST::ValueTypePrimitive::t_uint16;
    } else if (hex_len <= 8) {
        guessed_int_type = AST::ValueTypePrimitive::t_uint32;
    } else {
        guessed_int_type = AST::ValueTypePrimitive::t_uint64;
    }

    auto &node = payload.context.emplace_node<AST::LiteralIntExprNode>(current_token, guessed_int_type);

    // interpret the hex value as an integer
    if (guessed_int_type == AST::ValueTypePrimitive::t_uint64) {
        node.override_literal_value.emplace(std::to_string(std::stoull(hex_value, nullptr, 16)));
    } else if (guessed_int_type == AST::ValueTypePrimitive::t_uint32) {
        node.override_literal_value.emplace(std::to_string(std::stoul(hex_value.substr(0, 10), nullptr, 16)));
    } else if (guessed_int_type == AST::ValueTypePrimitive::t_uint16) {
        node.override_literal_value.emplace(std::to_string(std::stoul(hex_value.substr(0, 6), nullptr, 16)));
    } else if (guessed_int_type == AST::ValueTypePrimitive::t_uint8) {
        node.override_literal_value.emplace(std::to_string(std::stoul(hex_value.substr(0, 4), nullptr, 16)));
    }

    cursor.skip();
    return AST::make_ref(node);
}

/**
 * BINARY LITERAL
 * ----------------------------------------------------------------------------
 * 
 * Parse a binary literal and return a node reference to it.
 */
const AST::NodeReference parse_literal_binary(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;
    auto current_token = cursor.current();
}

/**
 * BOOLEAN LITERAL
 * ---------------------------------------------------------------------------- 
 * 
 * Parse a boolean literal and return a node reference to it.
 */
const AST::NodeReference parse_literal_boolean(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;
    auto current_token = cursor.current();

    if (current_token.type() != Token::Type::t_bool_literal) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
            payload.context.code_ref(current_token), 
            Token::Type::t_bool_literal, 
            current_token.type()
        );
        return AST::make_void_ref();
    }

    auto &node = payload.context.emplace_node<AST::LiteralBoolExprNode>(current_token);
    cursor.skip();

    // if there is a specified expected type, check if the literal fits the type
    if (expected_type != nullptr) 
    {
        // if we except a int type, we simply convert the boolean to an integer
        if (expected_type->type.is_integer_type())
        {
            auto &casted_node = payload.context.emplace_node<AST::LiteralIntExprNode>(current_token, expected_type->type.get_primitive_type());
            casted_node.override_literal_value.emplace(node.get_bool_value() ? "1" : "0");
            return AST::make_ref(casted_node);
        }
        else {
            payload.collector.collect_issue<AST::Issue::InvalidTypeConversion>(
                payload.context.code_ref(current_token), 
                std::format(
                    "The boolean literal '{}' cannot be implicitly converted to the expected type '{}'.", 
                    current_token.value(),
                    expected_type->type.get_type_desciption()
                )
            );
            return AST::make_void_ref();
        }
    }

    return AST::make_ref(node);
}

/**
 * Tries to implicitly cast the source expression to the expected type.
 */
AST::NodeReference try_implicit_cast(Parser::Payload &payload, AST::NodeReference source, const AST::ValueType &expected_type)
{
    // literal int?
    if (source.has_type<AST::LiteralIntExprNode>()) {
        auto &literal_node = source.get<AST::LiteralIntExprNode>();
        return autocast_literal_int(payload, literal_node, &expected_type);
    }
    // literal float?
    else if (source.has_type<AST::LiteralFloatExprNode>()) {
        auto &literal_node = source.get<AST::LiteralFloatExprNode>();
        return autocast_literal_float(payload, literal_node, &expected_type);
    }
    // other expressions
    else if (source.is_expression_node()) {
        auto expr_node = source.unsafe_ptr<AST::ExprNode>();

        // if the expression is already of the expected type, we can return it
        if (expr_node->result_type() == expected_type) {
            return source;
        }

        // otherwise we create a type cast node
        auto &cast_node = payload.context.emplace_node<AST::TypeCastNode>(expected_type, expr_node);
        return AST::make_ref(cast_node);
    }
    else {
        // payload.collector.collect_issue<AST::Issue::InvalidTypeConversion>(
        //     payload.context.code_ref(source.token_reference()), 
        //     std::format(
        //         "Cannot implicitly cast the expression of type '{}' to the expected type '{}'.", 
        //         source.result_type().get_type_desciption(),
        //         expected_type.get_type_desciption()
        //     )
        // );
        // return AST::make_void_ref();
    }

    return source;
}

/**
 * BINARY EXPRESSION
 * ----------------------------------------------------------------------------
 * 
 * Parse a binary expression, handles special cases like implicit casting
 * and returns a node reference to the resulting expression node.  
 */
const AST::NodeReference parse_binary_expr(Parser::Payload &payload, AST::OperatorNode *op_node, AST::NodeReference lhs, AST::NodeReference rhs)
{
    // they have to be expr nodes
    auto lhs_expr = lhs.unsafe_ptr<AST::ExprNode>();
    auto rhs_expr = rhs.unsafe_ptr<AST::ExprNode>();

    auto lhs_type = lhs_expr->result_type();
    auto rhs_type = rhs_expr->result_type();

    // if one of the nodes is has a integer return type and the other a floating one
    // we either convert the literal to a float or cast the referenced expression to a float
    if (lhs_type.is_integer_type() && rhs_type.is_floating_type()) {
        // convert the lhs to a float
        lhs = try_implicit_cast(payload, lhs, rhs_type);
    } else if (lhs_type.is_floating_type() && rhs_type.is_integer_type()) {
        // convert the rhs to a float
        rhs = try_implicit_cast(payload, rhs, lhs_type);
    }
    // two floating types? we cast to the larger one
    else if (
        lhs_type.is_floating_type() && rhs_type.is_floating_type() && 
        (lhs_type.get_primitive_type() != rhs_type.get_primitive_type())
    ){
        if (get_primitive_size(lhs_type.get_primitive_type()) > get_primitive_size(rhs_type.get_primitive_type())) {
            rhs = try_implicit_cast(payload, rhs, lhs_type);
        } else {
            lhs = try_implicit_cast(payload, lhs, rhs_type);
        }
    }
    // two integer types? we cast to the larger one
    // basically the same as above, i could merge it, but in the back of my mind
    // i think there will be some special rules.
    else if (
        lhs_type.is_integer_type() && rhs_type.is_integer_type() && 
        (lhs_type.get_primitive_type() != rhs_type.get_primitive_type())
    ) {
        if (get_primitive_size(lhs_type.get_primitive_type()) > get_primitive_size(rhs_type.get_primitive_type())) {
            rhs = try_implicit_cast(payload, rhs, lhs_type);
        } else {
            lhs = try_implicit_cast(payload, lhs, rhs_type);
        }
    }

    // update the expr ptrs
    lhs_expr = lhs.unsafe_ptr<AST::ExprNode>();
    rhs_expr = rhs.unsafe_ptr<AST::ExprNode>();

    auto &node = payload.context.emplace_node<AST::BinaryExprNode>(op_node, lhs_expr, rhs_expr);

    return AST::make_ref(node);
}

AST::ExprNode *Parser::parse_expr(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto ref = parse_expr_ref(payload, expected_type);

    // probably a bad idea, but it should never be not a expr node
    return ref.unsafe_ptr<AST::ExprNode>();
}

bool is_expr_token(Parser::Cursor &cursor)
{
    if (cursor.is_done()) {
        return false;
    }

    return cursor.is_type(Token::Type::t_floating_literal) ||
           cursor.is_type(Token::Type::t_integer_literal) ||
           cursor.is_type(Token::Type::t_hex_literal) ||
           cursor.is_type(Token::Type::t_binary_literal) ||
           cursor.is_type(Token::Type::t_bool_literal) ||
           cursor.is_type(Token::Type::t_varname) || 
           cursor.is_type(Token::Type::t_open_paren) || 
           cursor.is_type(Token::Type::t_close_paren) || 
           cursor.is_type(Token::Type::t_identifier) ||
           cursor.is_type(Token::Type::t_namespace_sep) ||
           cursor.is_type(Token::Type::t_string_literal) ||
           cursor.is_type(Token::Type::t_ref) ||
           // if the token has a operator precendence, it is a valid expression token
           AST::Operator::get_precedence_for_token(cursor.current().type()).sequence > 0;
}

const AST::NodeReference Parser::parse_member_chain(Parser::Payload &payload, AST::NodeReference base)
{
    auto &cursor = payload.cursor;
    auto current_ref = base;

    while (cursor.is_type(Token::Type::t_accessorlr)) {
        cursor.skip(); // skip the '->' token

        if (!cursor.is_type(Token::Type::t_identifier)) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_identifier, cursor.current().type());
            return AST::make_void_ref();
        }

        auto member_token = cursor.current();
        cursor.skip(); // skip the member name

        auto &member_access = payload.context.emplace_node<AST::MemberAccessNode>(current_ref, member_token);
        current_ref = AST::make_ref(member_access);
    }

    return current_ref;
}

const AST::NodeReference parse_expr_node(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;

    if (cursor.is_type(Token::Type::t_floating_literal)) {
        return parse_literal_float(payload, expected_type);
    }

    else if (cursor.is_type(Token::Type::t_integer_literal)) {
        return parse_literal_int(payload, expected_type);
    }

    else if (cursor.is_type(Token::Type::t_hex_literal)) {
        return parse_literal_hex(payload, expected_type);
    }

    else if (cursor.is_type(Token::Type::t_binary_literal)) {
        return parse_literal_binary(payload, expected_type);
    }

    else if (cursor.is_type(Token::Type::t_bool_literal)) {
        return parse_literal_boolean(payload, expected_type);
    }

    else if (cursor.is_type(Token::Type::t_string_literal)) {
        auto &node = payload.context.emplace_node<AST::LiteralStringExprNode>(cursor.current());
        cursor.skip();
        return AST::make_ref(node);
    }

    else if (
        cursor.is_type(Token::Type::t_varname) || 
        cursor.is_type_sequence(0, { Token::Type::t_ref, Token::Type::t_varname })
    ) {
        // if the token is a reference operator we have to handle it
        bool is_creating_ptr = cursor.is_type(Token::Type::t_ref);
        if (is_creating_ptr) {
            cursor.skip();
        }

        auto var_token = cursor.current();
        auto vardecl = payload.context.scope().find_vardecl_by_name(var_token.value());

        if (!vardecl) {
            payload.collector.collect_issue<AST::Issue::UnknownVariable>(payload.context.code_ref(cursor.current()), cursor.current().value());
            cursor.skip();
            return AST::make_void_ref();
        }

        cursor.skip(); // skip the variable name
        
        // Create the base variable reference
        auto &varnode = payload.context.emplace_node<AST::VarNode>(vardecl, var_token);
        auto &varref = payload.context.emplace_node<AST::VarRefNode>(&varnode);
        auto current_ref = AST::make_ref(varref);
        
        // wrap the base in a MemberAccessNode for each `->member` in the chain
        current_ref = Parser::parse_member_chain(payload, current_ref);
        if (!current_ref.has()) {
            return AST::make_void_ref();
        }

        if (is_creating_ptr) {
            // Create a pointer expression node
            auto &ptr_expr = payload.context.emplace_node<AST::VarPtrExprNode>(current_ref.get_ptr<AST::VarRefNode>());
            return AST::make_ref(ptr_expr);
        }
        
        return current_ref;
    }

    // there might be a namespace used 
    // like 
    //   math::sin(1.0)
    //   math::PI
    //   math::$foo 
    const AST::Namespace *ast_namespace = nullptr;
    if (cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_namespace_sep })) {
        auto ns_node = parse_namespace(payload);
        assert(ns_node != nullptr && "expected a namespace node");
        ast_namespace = ns_node->ast_namespace;
    }

    // potential function call - `name(...)` or, with explicit type arguments, `name<...>(...)`.
    // a bare identifier is never a comparison operand (values are $-prefixed), so an identifier
    // followed by '<' here is a generic call, not a less-than.
    if (
        cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_open_paren }) ||
        cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_open_angle })
    ) {
        auto fcall = parse_funccall(payload, ast_namespace);
        return AST::make_ref(fcall);
    }

    assert(false && "unimplemented");
}

// parse an operand appearing in prefix position, consuming any leading unary
// '-' / '+' operators and wrapping negations in a UnaryExprNode. unary '+' is
// a no-op and returns the operand unchanged
const AST::NodeReference parse_prefix_unary(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;

    auto op = payload.collector.operators.get_operator(cursor.current());
    if (op != nullptr && (op->type == Token::Type::t_op_sub || op->type == Token::Type::t_op_add)) {
        auto op_token = cursor.current();
        cursor.skip();

        // recurse so chained prefixes like `- -$x` resolve right to left
        auto operand = parse_prefix_unary(payload, expected_type);
        if (!operand.has()) {
            return AST::make_void_ref();
        }

        // unary plus carries no semantics
        if (op->type == Token::Type::t_op_add) {
            return operand;
        }

        auto &unary = payload.context.emplace_node<AST::UnaryExprNode>(
            op_token, operand.unsafe_ptr<AST::ExprNode>());
        return AST::make_ref(unary);
    }

    // a parenthesized subexpression, e.g. -(a + b)
    if (cursor.is_type(Token::Type::t_open_paren)) {
        cursor.skip();
        auto inner = Parser::parse_expr_ref(payload, expected_type);
        if (cursor.is_type(Token::Type::t_close_paren)) {
            cursor.skip();
        }
        return inner;
    }

    return parse_expr_node(payload, expected_type);
}

struct ExprPart {
    // val node
    const AST::NodeReference node;
    // operator node
    AST::OperatorNode *opnode;
};

#define O1Prec part.opnode->op->precedence
#define O2Prec operator_stack.top()->op->precedence

std::vector<ExprPart> shunting_yard(const std::vector<ExprPart> &expr_parts)
{
    auto output = std::vector<ExprPart>();
    auto operator_stack = std::stack<AST::OperatorNode *>();

    for(auto part : expr_parts)
    {
        // if its a literal, variable etc. (not an operator)
        if (part.opnode == nullptr) 
        {
            output.push_back(part);
        }
        else if (part.opnode->op->type == Token::Type::t_open_paren)
        {
            operator_stack.push(part.opnode);
        }
        else if (part.opnode->op->type == Token::Type::t_close_paren)
        {
            while (!operator_stack.empty() && operator_stack.top()->op->type != Token::Type::t_open_paren)
            {
                output.push_back({AST::make_void_ref(), operator_stack.top()});
                operator_stack.pop();
            }

            // ensure we have the opening "(", otherwise something is off
            assert(operator_stack.top()->op->type == Token::Type::t_open_paren);
            operator_stack.pop();
        }
        else
        {
            while (
                !operator_stack.empty() &&
                // O2Prec.assoc != AST::OpAssociativity::left &&
                operator_stack.top()->op->type != Token::Type::t_open_paren &&
                (
                    O2Prec.sequence < O1Prec.sequence ||
                    (
                        O1Prec.sequence == O2Prec.sequence &&
                        O1Prec.assoc == AST::OpAssociativity::left
                    )
                )
            ) {
                output.push_back({AST::make_void_ref(), operator_stack.top()});
                operator_stack.pop();
            }

            operator_stack.push(part.opnode);
        }        
    }

    while (!operator_stack.empty())
    {
        output.push_back({AST::make_void_ref(), operator_stack.top()});
        operator_stack.pop();
    }

    return output;
}

const AST::NodeReference Parser::parse_expr_ref(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;

    std::vector<ExprPart> expr_parts;

    int depth = 0;

    auto token = cursor.current();
    auto tvalue = token.value();

    while(is_expr_token(cursor))
    {
        // if we have a closing parenthesis and the depth is 0, we can break the loop
        // because we have reached the end of the expression
        if (cursor.is_type(Token::Type::t_close_paren) && depth == 0) {
            break;
        }

        // try to parse an operator
        auto op = payload.collector.operators.get_operator(cursor.current());

        // a '-' or '+' in operand position is a prefix unary operator, not a
        // binary one. detect it and parse the operand it applies to, otherwise
        // the shunting yard would hand parse_binary_expr a null lhs and crash
        bool expects_operand = expr_parts.empty() ||
            (expr_parts.back().opnode != nullptr &&
             expr_parts.back().opnode->op->type != Token::Type::t_close_paren);

        if (op != nullptr && expects_operand &&
            (op->type == Token::Type::t_op_sub || op->type == Token::Type::t_op_add))
        {
            auto node = parse_prefix_unary(payload, expected_type);
            if (!node.has()) {
                return AST::make_void_ref();
            }
            expr_parts.emplace_back(node, nullptr);
            continue;
        }

        auto &opnode = payload.context.emplace_node<AST::OperatorNode>(cursor.current(), op);

        if (op != nullptr) {
            cursor.skip();
            expr_parts.emplace_back(AST::make_void_ref(), &opnode);

            // if the operator is a open parenthesis, we increase the depth
            if (op->type == Token::Type::t_open_paren) {
                depth++;
            } else if (op->type == Token::Type::t_close_paren) {
                depth--;
            }

            continue;
        }

        // parse the next expression node
        auto node = parse_expr_node(payload, expected_type);
        
        // if the node is empty 
        if (!node.has()) {
            return AST::make_void_ref();
        }
        
        expr_parts.emplace_back(node, nullptr);
    }

    // if we have only one part, we can return it directly
    if (expr_parts.size() == 1) {
        assert(expr_parts[0].opnode == nullptr && "expected no operator");
        return expr_parts[0].node;
    }

    auto postfix_expr = shunting_yard(expr_parts);

    // build expressions nodes
    std::stack<AST::NodeReference> node_stack;
    for (auto &part : postfix_expr) 
    {
        if (part.opnode != nullptr) 
        {
            auto right = node_stack.top();
            node_stack.pop();

            auto left = node_stack.top();
            node_stack.pop();

            // let our binary expresssion parser take over
            // there is not much to parse here but it will handle type casts 
            // and other node transformation to ensure echos expression behavior
            node_stack.push(parse_binary_expr(
                payload, 
                part.opnode, 
                left, 
                right
            ));
        }
        else 
        {
            node_stack.push(part.node);
        }
    }

    // sanity check
    assert(node_stack.size() == 1);
    return node_stack.top();

    // // print the postfix expression
    // for (auto &part : postfix_expr) {
    //     if (part.opnode != nullptr) {
    //         std::cout << std::format("{} ", token_lit_symbol_string(part.opnode->op->type));
    //     }
    //     else {
    //         if (part.node.has_type<AST::LiteralIntExprNode>()) {
    //             std::cout << std::format("{} ", part.node.get<AST::LiteralIntExprNode>().effective_token_literal_value());
    //         } else if (part.node.has_type<AST::LiteralFloatExprNode>()) {
    //             std::cout << std::format("{} ", part.node.get<AST::LiteralFloatExprNode>().effective_token_literal_value());
    //         } else if (part.node.has_type<AST::LiteralBoolExprNode>()) {
    //             std::cout << std::format("{} ", part.node.get<AST::LiteralBoolExprNode>().effective_token_literal_value());
    //         } else {
    //             std::cout << std::format("{} ", part.node.node()->node_description());
    //         }
    //     }
    // }


    // // determine the token range of the expression
    // while (!cursor.is_done()) {
    //     if (!is_expr_token(cursor)) {
    //         break;
    //     }
    //     cursor.skip();
    // }

    // auto cursor_after = cursor.snapshot();
    // auto expr_slice = cursor.slice(cursor_before, cursor_after);
    // cursor.restore(cursor_before);

    // // collect the tokens in range and perform the shunting yard algorithm
    // // to create a postfix expression
    // auto postfix_expr = AST::Operator::shunting_yard(expr_slice);


    // if (cursor.is_type(Token::Type::t_floating_literal)) {
    //     return parse_literal_float(payload, expected_type);
    // }

    // if (cursor.is_type(Token::Type::t_integer_literal)) {
    //     return parse_literal_int(payload, expected_type);
    // }

    // if (cursor.is_type(Token::Type::t_bool_literal)) {
    //     auto &node = payload.context.emplace_node<AST::LiteralBoolExprNode>(cursor.current());
    //     cursor.skip();
    //     return AST::make_ref(node);
    // }

    // if (cursor.is_type(Token::Type::t_varname)) {
    //     auto vardecl = payload.context.scope().find_vardecl_by_name(cursor.current().value());

    //     if (!vardecl) {
    //         payload.collector.collect_issue<AST::Issue::UnknownVariable>(payload.context.code_ref(cursor.current()), cursor.current().value());
    //         cursor.skip();
    //         return AST::make_void_ref();
    //     }   

    //     auto &varref = payload.context.emplace_node<AST::VarRefNode>(cursor.current(), vardecl);
    //     auto &node = payload.context.emplace_node<AST::VarRefExprNode>(&varref);
    //     cursor.skip();
        
    //     return AST::make_ref(node);
    // }

    assert(false && "unimplemented");
}