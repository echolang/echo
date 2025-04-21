#include "Parser/ExprParser.h"

#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTOps.h"
#include "AST/ASTNullability.h"
#include "AST/ExprNode.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/VarRefNode.h"
#include "AST/VarNode.h"
#include "AST/OperatorNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/ASTStringLiteral.h"
#include "AST/TypeCastNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/ASTMemberLookup.h"
#include "AST/NullNode.h"

#include "External/infint.h"

#include "Parser/FuncCallParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/NamespaceParser.h"
#include "Parser/TypeParser.h"
#include "AST/TypeNode.h"

#include <fmt/core.h>
#include <stack>

bool can_hold_literal_int(Parser::Payload &payload, AST::ValueType type, const std::string &literal, const TokenReference literal_token)
{
    InfInt value(literal);

    auto int_size = AST::get_integer_size(type.get_primitive_type());

    if (value > int_size.get_max_positive_value()) {
        payload.collector.collect_issue<AST::Issue::IntegerOverflow>(
            payload.context.code_ref(literal_token), 
            fmt::format(
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
            fmt::format(
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

// the comma separated expressions between a `[` and its `]`, cursor already past the opening bracket
// and left after the closing one. answers false having reported when the list does not close
//
// **one grammar for what may sit inside brackets**, because two positions ask: an index list
// (`$m[$r, $c]`) and an array literal (`[1, 2, 3]`). an empty list is legal at both - `$a[]` is the
// append slot and `[]` is an empty literal - so emptiness is the caller's question, not this one's
bool parse_bracketed_expr_list(Parser::Payload &payload, std::vector<AST::ExprNode *> &elements)
{
    auto &cursor = payload.cursor;

    while (!cursor.is_type(Token::Type::t_close_bracket)) {
        auto *element = Parser::parse_expr(payload, nullptr);

        if (element == nullptr) {
            return false;
        }

        elements.push_back(element);

        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
            continue;
        }

        break;
    }

    if (!cursor.is_type(Token::Type::t_close_bracket)) {
        payload.collect_unexpected_token(Token::Type::t_close_bracket);
        return false;
    }

    cursor.skip(); // `]`

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

    // only a concrete primitive can say what a literal is (AST::can_type_a_literal)
    if (expected_type != nullptr && !AST::can_type_a_literal(*expected_type)) {
        expected_type = nullptr;
    }

    // if there is a specified expected type, check if the literal fits the type
    if (expected_type != nullptr) {
        // floats / doubles
        if (expected_type->is_floating_type()) {
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
                        fmt::format(
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
        else if (expected_type->is_integer_type()) {
            // determine if the literal has any decimal values besides 0
            // if so, we emit a error (not just a warning) because the user highly likely made a mistake
            // or is expecting a wrong type
            double dliteral = std::stod(node.get_fvalue_string());
            double dliteral_cmp = (double) (long long) dliteral;

            if (dliteral != dliteral_cmp) {
                payload.collector.collect_issue<AST::Issue::InvalidTypeConversion>(
                    payload.context.code_ref(literal_token), 
                    fmt::format(
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

    // only a concrete primitive can say what a literal is (AST::can_type_a_literal)
    if (expected_type != nullptr && !AST::can_type_a_literal(*expected_type)) {
        expected_type = nullptr;
    }

    if (expected_type != nullptr) {
        // floats / doubles
        // if the expected type is a float, we can "safely" convert the integer to a float
        if (expected_type->is_floating_type()) {
            // we can safely convert the integer to a float
            auto &casted_node = payload.context.emplace_node<AST::LiteralFloatExprNode>(literal_token, expected_type->get_primitive_type());

            if (expected_type->get_primitive_type() == AST::ValueTypePrimitive::t_float32) {
                float val = casted_node.float_value();
                casted_node.override_literal_value.emplace(get_f32_string_literal(val));
            } 
            else if (expected_type->get_primitive_type() == AST::ValueTypePrimitive::t_float64) {
                double val = casted_node.double_value();
                casted_node.override_literal_value.emplace(get_f64_string_literal(val));
            } 
            else {
                payload.collector.collect_issue<AST::Issue::InvalidTypeConversion>(
                    payload.context.code_ref(literal_token), 
                    fmt::format(
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
        else if (expected_type->is_integer_type()) {
            auto &expected_node = payload.context.emplace_node<AST::LiteralIntExprNode>(literal_token, expected_type->get_primitive_type());

            // check if the expected type is unsigned and the literal is negative
            // which should throw an error
            if (expected_type->is_unsigned_integer() && intvalue < 0) {
                payload.collector.collect_issue<AST::Issue::InvalidTypeConversion>(
                    payload.context.code_ref(literal_token), 
                    fmt::format(
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
                    fmt::format(
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
                    fmt::format(
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
        else if (expected_type->is_boolean_type()) {
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
        payload.collect_unexpected_token(Token::Type::t_bool_literal);
        return AST::make_void_ref();
    }

    auto &node = payload.context.emplace_node<AST::LiteralBoolExprNode>(current_token);
    cursor.skip();

    // only a concrete primitive can say what a literal is (AST::can_type_a_literal)
    if (expected_type != nullptr && !AST::can_type_a_literal(expected_type->type)) {
        expected_type = nullptr;
    }

    // if there is a specified expected type, check if the literal fits the type
    if (expected_type != nullptr) {
        // the literal already is what was asked for. without this the branch below reported that
        // the boolean literal 'true' cannot be converted to the expected type 'bool' - it was
        // only reachable through a pointer destination before a `return` started supplying its
        // declared type as the expected one
        if (expected_type->type.is_boolean_type()) {
            return AST::make_ref(node);
        }

        // if we except a int type, we simply convert the boolean to an integer
        if (expected_type->type.is_integer_type()) {
            auto &casted_node = payload.context.emplace_node<AST::LiteralIntExprNode>(current_token, expected_type->type.get_primitive_type());
            casted_node.override_literal_value.emplace(node.get_bool_value() ? "1" : "0");
            return AST::make_ref(casted_node);
        }
        else {
            payload.collector.collect_issue<AST::Issue::InvalidTypeConversion>(
                payload.context.code_ref(current_token), 
                fmt::format(
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
        //     fmt::format(
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

    if (lhs_expr == nullptr || rhs_expr == nullptr) {
        return AST::make_void_ref();
    }

    // read once, and read here: both the declared-operator gate below and every built-in arm under it
    // want them, and result_type() walks the operand's subtree - a member access resolves its property
    // by name - so asking twice is asking a question that cannot answer differently
    auto lhs_type = lhs_expr->result_type();
    auto rhs_type = rhs_expr->result_type();

    // **`??` is not a binary operator**, though it is spelled and parsed as one. it short-circuits, so it
    // cannot be a function of two already-evaluated operands the way every other infix symbol here is -
    // the right side must not run when the left is there. it gets its own node and its own lowering
    //
    // ahead of the declared-operator gate below because `??` is not declarable: it is spelled by the
    // language, and letting an overload set claim the symbol would make the short circuit optional
    if (op_node != nullptr && op_node->op != nullptr
        && op_node->op->type == Token::Type::t_qmark_qmark) {

        // the weak upgrade, through the one function all three forms share. after this the left side is an
        // ordinary nullable and everything below knows nothing about weak references
        AST::ExprNode *left = AST::optional_operand_of(
            lhs_expr, payload.context.module, op_node->token_literal);

        const AST::ValueType left_type = left->result_type();

        // a left side that can never be absent makes the right side dead code. reported rather than
        // folded away, for `guard`'s reason: it reads as a claim about the value, and a claim that is
        // always false is a mistake somewhere. an undetermined type waits for a later round as ever
        if (AST::is_certainly_present(left_type)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(op_node->token_literal),
                fmt::format(
                    "'??' needs a value that may be absent on its left, and '{}' always is one - the "
                    "right side could never be reached",
                    left_type.get_type_desciption()));
            return AST::make_void_ref();
        }

        auto &node = payload.context.emplace_node<AST::NullCoalesceExprNode>(
            left, rhs_expr, op_node->token_literal);

        return AST::make_ref(node);
    }

    // **a declared operator, before the built-in arms below.** an operator is a function, so all this
    // does is hand the operands to the same overload resolution every call goes through - which is
    // why nothing downstream of here has an operator case at all
    //
    // the gate is asked of the *operator table*, filled by the type-name pass, rather than of the
    // overload set, filled by the declaration pass: this function runs during the declaration pass
    // too, for a struct property's `= ...` initializer, and asking a half-filled overload set there
    // would answer differently depending on which file was walked first
    if (op_node != nullptr && op_node->op != nullptr
        && op_node->op->has_fixity(AST::OpFixity::t_infix)
        && !AST::binary_has_builtin_meaning(
            op_node->op,
            AST::parse_time_operand(lhs_expr, lhs_type),
            AST::parse_time_operand(rhs_expr, rhs_type))) {

        auto *call = Parser::build_operator_call(
            payload, *op_node->op, AST::OpFixity::t_infix, op_node->token_literal,
            {lhs_expr, rhs_expr});

        if (call == nullptr) {
            return AST::make_void_ref();
        }

        return AST::make_ref(*call);
    }

    // **the numeric reconciliation, asked of AST::common_numeric_type** - which AST::OperatorRewriter
    // asks again for the operands only a later pass gives a type to. the rule is shared; the insertion
    // is not, and this is the moment that can do better than a cast: try_implicit_cast retypes a literal
    // outright where the rewriter has nothing left but a TypeCastNode to wrap the operand in
    if (const auto common = AST::common_numeric_type(lhs_type, rhs_type)) {
        // exactly one side differs: the common type is always one of the two
        if (lhs_type.get_primitive_type() != common->get_primitive_type()) {
            lhs = try_implicit_cast(payload, lhs, *common);
        } else {
            rhs = try_implicit_cast(payload, rhs, *common);
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

// a declared operator matching at the cursor, or {} - the sequence lookup the yard, the prefix
// position and the suffix chain all go through, so "is there an operator here" is asked one way
//
// bounded by the cursor's range, because a module's token collection holds every one of its files
// back to back and a multi-token symbol at the end of one would otherwise match into the next
AST::OperatorRegistry::Match operator_match_at(Parser::Payload &payload, Parser::Cursor &cursor)
{
    if (cursor.is_done()) {
        return {};
    }

    return payload.collector.operators.match_at(cursor.current(), cursor.remaining());
}

// the **declared** operator with this fixity at the cursor, or {}. the fixity is part of the question
// because the three positions are different: a suffix-only `mm` must not be tried as an infix
// operator, and a bare identifier must not become an operator just because some symbol somewhere is
// spelled that way
//
// answers with the match rather than a bool because a caller that goes on to consume the symbol needs
// its token_count, and asking twice means matching twice - a sequence scan per declared symbol, at a
// position the parser is already standing on
AST::OperatorRegistry::Match declared_operator_at(
    Parser::Payload &payload, Parser::Cursor &cursor, AST::OpFixity fixity)
{
    const auto match = operator_match_at(payload, cursor);

    // has_fixity implies is_declared - the fixities *are* the declarations, so asking both reads as
    // two facts where there is one
    if (!match.has() || !match.op->has_fixity(fixity)) {
        return {};
    }

    return match;
}

// the same question where only the answer's existence is wanted
bool starts_declared_operator(Parser::Payload &payload, Parser::Cursor &cursor, AST::OpFixity fixity)
{
    return declared_operator_at(payload, cursor, fixity).has();
}

bool is_expr_token(Parser::Payload &payload, Parser::Cursor &cursor)
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
           cursor.is_type(Token::Type::t_ptr_of) ||
           cursor.is_type(Token::Type::t_null) ||
           // `ptr<T>(...)` is a cast, so a type keyword can begin an expression
           cursor.is_type(Token::Type::t_ptr) ||
           // and `weak($obj)` / `strong($w)` for the same reason. without these the shunting-yard loop
           // never enters, the expression comes back empty, and parse_expr_ref's sanity assert fires -
           // which is the trap `mv` and the closure literal below already document
           cursor.is_type(Token::Type::t_weak) ||
           cursor.is_type(Token::Type::t_strong) ||
           // `mv E` is a prefix operator, so it begins one too. without this the shunting-yard loop
           // never enters and the expression comes back empty
           cursor.is_type(Token::Type::t_mv) ||
           // a closure literal, `function(...) { ... }`. the same trap as `mv` above: without this the
           // loop does not enter and the expression comes back empty. guarded on the `(` so the
           // callable *type* `function<...>` - which is not an expression - cannot get in here
           Parser::starts_closure_literal(cursor) ||
           // `$a instanceof Foo` continues an expression that already began, so the loop must not
           // stop at the keyword - parse_postfix_chain is what actually consumes it
           cursor.is_type(Token::Type::t_instanceof) ||
           cursor.is_type(Token::Type::t_open_bracket) ||
           // if the token has a operator precendence, it is a valid expression token
           AST::Operator::get_precedence_for_token(cursor.current().type()).sequence > 0 ||
           // a declared **prefix** operator, which may be spelled out of tokens nothing else in this
           // list admits - `!!` is two t_exclamation, and neither has a precedence. without this the
           // loop below never enters for `echo !!'hello';`, `expr_parts` stays empty, and the
           // sanity assert at the end of parse_expr_ref takes the compiler down
           //
           // gated to prefix fixity so a bare identifier - which is already admitted above, as the
           // start of a call - does not change meaning just because some suffix operator is spelled
           // that way somewhere in the program
           //
           // **last**, because it is the only arm that costs a lookup: the precedence test above is a
           // switch on the token type and answers for every built-in operator token, so only a token
           // that is nothing else in this list reaches the symbol trie
           starts_declared_operator(payload, cursor, AST::OpFixity::t_prefix);
}

// `( expr )`, the shape every call-like form here is written in - `weak(...)`, `strong(...)`, a pointer
// cast. one helper rather than three copies, so the recovery is the same wherever the parens are: a
// located issue at whichever bracket was missing, and nullptr for the caller to recover from
AST::ExprNode *parse_parenthesized_operand(Parser::Payload &payload, AST::TypeNode *expected_type = nullptr)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type(Token::Type::t_open_paren)) {
        payload.collect_unexpected_token(Token::Type::t_open_paren);
        return nullptr;
    }
    cursor.skip();

    auto *operand = Parser::parse_expr(payload, expected_type);
    if (operand == nullptr) {
        return nullptr;
    }

    if (!cursor.is_type(Token::Type::t_close_paren)) {
        payload.collect_unexpected_token(Token::Type::t_close_paren);
        return nullptr;
    }
    cursor.skip();

    return operand;
}

AST::ExprNode *Parser::parse_weak_expr(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;
    auto weak_token = cursor.current();

    // the explicit form goes through **Parser::parse_type**, the one owner of the type grammar, rather
    // than re-reading `< type >` here. so `weak<a::b::Foo>` and `weak<Box<int32>>` mean the same in an
    // expression as in a declaration, including the `>>` split, and the class-only rule is enforced once
    std::optional<AST::ValueType> written_target;
    if (cursor.is_type_sequence(0, { Token::Type::t_weak, Token::Type::t_open_angle })) {
        AST::TypeNode *written = Parser::parse_type(payload);
        if (written == nullptr) {
            return nullptr;
        }

        // parse_type already reported anything that is not a weak - a `weak<int32>` collects its issue at
        // the type and answers nullopt there, so reaching here with something else is not possible
        if (!written->type.is_weak()) {
            return nullptr;
        }

        written_target = written->type.weak_target();
    }
    else {
        cursor.skip(); // `weak`
    }

    auto *operand = parse_parenthesized_operand(payload);
    if (operand == nullptr) {
        return nullptr;
    }

    const AST::ValueType operand_type = operand->result_type();

    // a weak reference needs the object's *handle*, which means somewhere to read it from. a temporary
    // would be released the moment the statement ended, so the weak would be dead before it was named -
    // refusing here says that, rather than letting the program discover it at runtime
    if (!AST::is_place_expression(*operand)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(weak_token),
            "'weak' needs an expression with storage to reference - a value nothing owns is already "
            "gone by the time a weak reference to it could be read");
        return nullptr;
    }

    // an undetermined operand is "ask again later", the standing rule for a type no round has answered.
    // the node's own result_type() re-asks after substitution, so a `&$t` inside a template needs no
    // decision here either
    if (!operand_type.is_class() && !AST::is_undetermined_type(operand_type)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(weak_token),
            fmt::format(
                "'weak' needs a class, not '{}' - only a class is reference counted, so only a class "
                "has a count to opt out of",
                operand_type.get_type_desciption()));
        return nullptr;
    }

    // checked against the operand, never used to change it: the target of a weak reference is whatever
    // the operand already is. so a mismatch is a claim the program got wrong rather than a conversion
    if (written_target.has_value() && operand_type.is_class()
        && !(written_target.value() == operand_type)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(weak_token),
            fmt::format(
                "'weak<{}>' does not match its operand's type '{}' - a weak reference names the class "
                "it was taken of, so the two cannot differ",
                written_target->get_type_desciption(), operand_type.get_type_desciption()));
        return nullptr;
    }

    // **the same node a `&` with a weak destination builds.** `weak($obj)` is not a second operation, it is
    // the same one saying so itself instead of reading it off a destination - so ownership, lowering and the
    // dump all follow one path and cannot come apart. it is also the spelling for the case the destination
    // rule cannot serve: an *inferred* `$w = weak($a)` has no declared type to ask
    return &payload.context.emplace_node<AST::AddrOfExprNode>(operand, true);
}

AST::ExprNode *Parser::parse_strong_expr(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;
    auto strong_token = cursor.current();
    cursor.skip(); // `strong`

    auto *operand = parse_parenthesized_operand(payload);
    if (operand == nullptr) {
        return nullptr;
    }

    // no place requirement and no type check here: the operand is read as a *value*, so a weak a call
    // returned upgrades as readily as one in a variable, and the "is it a weak" question belongs to
    // AST::TypeChecker - which asks it after the monomorphizer has settled every type
    return &payload.context.emplace_node<AST::StrongExprNode>(operand, strong_token);
}

const AST::NodeReference Parser::parse_postfix_chain(Parser::Payload &payload, AST::NodeReference base)
{
    auto &cursor = payload.cursor;
    auto current_ref = base;

    // **the `?->` links opened in this chain, innermost last.** each one swaps the marker in for the base
    // and records what it has to wrap; the whole lot is unwound after the loop, in reverse
    //
    // deferred rather than wrapped on the spot, and that is what makes `$a?->b->c` mean what a reader
    // expects: everything after the `?->` becomes part of *one* short circuit, so an absent `$a` skips the
    // `->c` as well. wrapping immediately would have left `->c` applied to a `B?`, which is refused - and
    // would have forced a `?->` at every link whether or not that link could be absent
    struct PendingOptionalLink
    {
        AST::ExprNode *base;
        AST::ChainBaseNode *marker;
        TokenReference token;
    };
    std::vector<PendingOptionalLink> optional_links;

    // one loop for every suffix that binds tighter than any operator. `->`, `?->` and `:$` live here
    // rather than in the shunting yard, which has no notion of a postfix operator and pops two
    // operands for everything it sees. `[...]` joins them later
    while (cursor.is_type(Token::Type::t_accessorlr)
        || cursor.is_type(Token::Type::t_optional_arrow)
        || cursor.is_type(Token::Type::t_ptr_of)
        || cursor.is_type(Token::Type::t_instanceof)
        || cursor.is_type(Token::Type::t_open_bracket)) {

        // `?->` opens a short circuit and then reads a member exactly as `->` does. so it is handled by
        // *substitution* rather than by a second member-access implementation: the base is replaced with a
        // marker standing for its unwrapped self, and the `->` arm below carries on unchanged
        if (cursor.is_type(Token::Type::t_optional_arrow)) {
            auto optional_token = cursor.current();
            auto *base = current_ref.unsafe_ptr<AST::ExprNode>();

            // the weak upgrade, through the one function all three forms share
            base = AST::optional_operand_of(base, payload.context.module, optional_token);

            const AST::ValueType base_type = base->result_type();

            // a base that is always there makes the `?` a lie - the short circuit could never fire, and
            // the reader is being told to expect an absence that cannot happen. `->` is what they want
            if (AST::is_certainly_present(base_type)) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(optional_token),
                    fmt::format(
                        "'?->' needs a value that may be absent, and '{}' always is one - write '->'",
                        base_type.get_type_desciption()));
                return AST::make_void_ref();
            }

            auto &marker = payload.context.emplace_node<AST::ChainBaseNode>(
                AST::unwrapped_type_of(base_type), optional_token);

            optional_links.push_back({base, &marker, optional_token});

            // from here the loop reads an ordinary `->` off the marker. the token is *not* consumed:
            // the arm below consumes it, and both spellings mean the same thing to it
            current_ref = AST::make_ref(marker);
        }

        // `E instanceof T`. here rather than in the shunting yard for a reason the other suffixes only
        // share by accident: its right operand is a *type*, not an expression, so there is no operand
        // for the yard to pop. binding as tightly as `->` also makes `$a instanceof Foo == true` read
        // the way it looks, with the comparison applied to the answer
        if (cursor.is_type(Token::Type::t_instanceof)) {
            auto instanceof_token = cursor.current();
            cursor.skip();

            auto *queried = Parser::parse_type(payload);
            if (queried == nullptr) {
                return AST::make_void_ref();
            }

            auto &check = payload.context.emplace_node<AST::InstanceOfExprNode>(
                current_ref.unsafe_ptr<AST::ExprNode>(), queried->type, instanceof_token);

            current_ref = AST::make_ref(check);
            continue;
        }

        if (cursor.is_type(Token::Type::t_open_bracket)) {
            auto bracket_token = cursor.current();
            cursor.skip();

            auto *base = current_ref.unsafe_ptr<AST::ExprNode>();

            // **an element is addressed, so the container has to be storage.** a place is, and so is
            // `$p:$`, which names an address directly - nothing else. the message is the one the
            // shunting yard's fallback still gives a literal or an array literal, and it is asked here
            // as well because a *call* now reaches this loop: `make()[0]` would otherwise be resolved
            // against the element contract, and fail as an overload that does not exist rather than as
            // the missing storage it is. giving a call result somewhere to live is todo/A13c, and needs
            // AST::argument_fit to rank a non-place against the contract's borrow parameter
            if (!AST::is_place_expression(*base) && base->get_node_type() != AST::NodeType::n_expr_peel) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(bracket_token),
                    "only a place can be indexed - a variable, a field or an element. Bind this "
                    "expression to a name first, then index that.");
                return AST::make_void_ref();
            }

            // **an empty `[]` is legal here**, and means the slot after the last one. it is not read
            // as "an index that failed to parse": AST::OperatorRewriter resolves it against the
            // container's one-operand `operator []`, and Parser refuses it anywhere but an
            // assignment target - see AST::IndexExprNode::is_append
            std::vector<AST::ExprNode *> indices;

            if (!parse_bracketed_expr_list(payload, indices)) {
                return AST::make_void_ref();
            }

            auto &index_expr = payload.context.emplace_node<AST::IndexExprNode>(
                base, std::move(indices), bracket_token);

            // the `:$` marker is erased by AST::PointerAdjuster long before the pass that has to
            // know, so whether one was written is recorded here, where it is still in the tree
            index_expr.base_was_peeled = base->get_node_type() == AST::NodeType::n_expr_peel;

            current_ref = AST::make_ref(index_expr);
            continue;
        }

        if (cursor.is_type(Token::Type::t_ptr_of)) {
            auto peel_token = cursor.current();
            cursor.skip();

            auto *operand = current_ref.unsafe_ptr<AST::ExprNode>();

            // `:$` walks one level outward each time. applied to an already peeled expression
            // there is no transparency left to strip, so it means the address of the slot -
            // which makes `$out:$:$` identical to `&$out` rather than a special case
            // (book/concept/pointers_and_refs_v2.md, "Pointers to pointers")
            if (operand->get_node_type() == AST::NodeType::n_expr_peel) {
                // no weak reading here, and it needs no destination to decide: `:$` requires a pointer
                // operand, and a class handle is not one - so `$out:$:$` is always the slot's address,
                // which is exactly the `&$out` it is documented to be identical to
                auto &addr = payload.context.emplace_node<AST::AddrOfExprNode>(
                    static_cast<AST::PointerValueNode *>(operand)->operand);
                current_ref = AST::make_ref(addr);
                continue;
            }

            if (!AST::is_place_expression(*operand)) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(peel_token),
                    "':$' needs an expression with storage to reach the pointer of");
                return AST::make_void_ref();
            }

            auto &peel = payload.context.emplace_node<AST::PointerValueNode>(operand, peel_token);
            current_ref = AST::make_ref(peel);
            continue;
        }

        auto accessor_token = cursor.current();
        cursor.skip(); // skip the '->' token

        // `->` already reaches through every pointer level, so `$p:$->x` could only ever mean what
        // `$p->x` means. it is rejected rather than aliased: `:$` marks an operation *on the
        // address*, and the pointer object itself has no members
        // (book/concept/pointers_and_refs_v2.md, "Structs and classes")
        if (current_ref.type() == AST::NodeType::n_expr_peel) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(accessor_token),
                "':$' names the pointer itself, which has no members - drop the ':$' and write '->' directly");
            return AST::make_void_ref();
        }

        if (!cursor.is_type(Token::Type::t_identifier)) {
            payload.collect_unexpected_token(Token::Type::t_identifier);
            return AST::make_void_ref();
        }

        auto member_token = cursor.current();
        cursor.skip(); // skip the member name

        // `->name(` where `name` is a *property* of callable type is a call through that value, not a
        // method call: `$h->op(21)` on a `function<int32(int32)> $op`.
        //
        // tried before the member-call attempt below, and gated on the type having no member function of
        // that name, so no existing call changes meaning. it has to come first rather than as a fallback,
        // because parse_member_call *commits* once a `(` follows - it reports an unknown member instead of
        // restoring, which is the right behaviour for a name that was meant to be a method
        if (cursor.is_type(Token::Type::t_open_paren)) {
            const AST::ValueType base_type =
                AST::target_type_of(current_ref.unsafe_ptr<AST::ExprNode>()->result_type());

            // the property is asked for first even though the member-function gate is the more
            // interesting condition: find_property is an O(1) map hit, where find_member_functions is a
            // linear scan that builds a vector. `->name(` is overwhelmingly an ordinary method call, and
            // in that shape there is no callable property, so this way it never pays for the scan
            const AST::ComplexType::Property *property = base_type.has_property_layout()
                ? base_type.get_complex_type()->find_property(member_token.value())
                : nullptr;

            if (property != nullptr && property->type.is_callable()
                && AST::find_member_functions(base_type.get_complex_type(), member_token.value()).empty())
            {
                auto &member_access =
                    payload.context.emplace_node<AST::MemberAccessNode>(current_ref, member_token);

                auto *call = Parser::parse_indirect_call(payload, &member_access, member_token);

                if (call == nullptr) {
                    return AST::make_void_ref();
                }

                current_ref = AST::make_ref(*call);
                continue;
            }
        }

        // `->name(` is a method call rather than a member read. `->name<` may be either: unlike a
        // free call, where a bare identifier can never be a comparison operand because values carry
        // a `$`, a member *is* a legitimate operand and `$a->count < 3` has to keep working. so the
        // type argument list is parsed speculatively and only committed when a `(` follows it
        if (cursor.is_type(Token::Type::t_open_paren) || cursor.is_type(Token::Type::t_open_angle)) {
            bool is_call = false;
            auto *call = Parser::parse_member_call(
                payload, current_ref.unsafe_ptr<AST::ExprNode>(), member_token, is_call);

            if (call != nullptr) {
                current_ref = AST::make_ref(*call);
                continue;
            }

            // a call that failed to resolve has already reported. reinterpreting its tokens as a
            // member read would report the same name a second time, from the type checker
            if (is_call) {
                return AST::make_void_ref();
            }

            // otherwise the `<` was a comparison: the cursor is back on it and this falls through to
            // the member read below, which is what `$a->count < 3` needs
        }

        auto &member_access = payload.context.emplace_node<AST::MemberAccessNode>(current_ref, member_token);
        current_ref = AST::make_ref(member_access);
    }

    // **the short circuits close here, innermost first.** everything the loop built after a `?->` is that
    // link's continuation, so unwinding in reverse puts each chain node around exactly the part of the
    // expression its base guards - and `$a?->b?->c` nests, stopping at whichever link is absent first
    for (auto link = optional_links.rbegin(); link != optional_links.rend(); ++link) {
        auto &chain = payload.context.emplace_node<AST::OptionalChainExprNode>(
            link->base, current_ref.unsafe_ptr<AST::ExprNode>(), link->marker, link->token);

        current_ref = AST::make_ref(chain);
    }

    return current_ref;
}

// consumes every declared **suffix** operator following an operand, innermost first, so
// `1m + 50cm` reads its units before the addition and `"x"_handle` is the whole operand
//
// here rather than in the shunting yard for parse_postfix_chain's reason - the yard pops two operands
// for everything it sees - and its own function rather than an arm of that chain because it has a
// second caller: a literal does not route through the postfix chain, and `1m` needs it to
//
// binding tighter than every binary operator is what the chapter's "the precedence of unary operators
// is higher than that of binary operators" means, and it is structural here rather than a number
const AST::NodeReference parse_suffix_operator_chain(Parser::Payload &payload, AST::NodeReference base)
{
    auto &cursor = payload.cursor;
    auto current_ref = base;

    while (current_ref.has()) {
        // one match per turn: the loop both decides on the symbol and consumes it, and those were two
        // separate lookups of the same position
        const auto match = declared_operator_at(payload, cursor, AST::OpFixity::t_suffix);

        if (!match.has()) {
            break;
        }

        const TokenReference symbol_token = cursor.current();

        auto *operand = current_ref.unsafe_ptr<AST::ExprNode>();
        if (operand == nullptr) {
            return AST::make_void_ref();
        }

        cursor.skip(match.token_count);

        auto *call = Parser::build_operator_call(
            payload, *match.op, AST::OpFixity::t_suffix, symbol_token, {operand});

        if (call == nullptr) {
            return AST::make_void_ref();
        }

        current_ref = AST::make_ref(*call);
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

    // `[1, 2, 3]`. the elements are parsed with no expected type of their own: each one is typed
    // where it lands, by the append the rewriter expands this into, which is an ordinary assignment
    // into a `T` slot - so `autocast_literal_int` and `coerce_value` do the work they already do
    else if (cursor.is_type(Token::Type::t_open_bracket)) {
        const auto bracket_token = cursor.current();
        cursor.skip();

        std::vector<AST::ExprNode *> elements;

        if (!parse_bracketed_expr_list(payload, elements)) {
            return AST::make_void_ref();
        }

        auto &literal = payload.context.emplace_node<AST::ArrayLiteralExprNode>(
            std::move(elements), bracket_token);

        return AST::make_ref(literal);
    }

    else if (cursor.is_type(Token::Type::t_null)) {
        // null has no type of its own - it takes the one the position expects. an unbound null
        // stays untyped here and is reported by the checker, which has the context to say so
        auto &node = payload.context.emplace_node<AST::NullNode>(cursor.current());

        // **any destination that admits absence**, which is one question rather than a list of kinds:
        // a `ptr<T>`, a `weak<T>`, and any `T?` whatever T is. it used to be `is_pointer() || is_class()`,
        // from when a class was implicitly nullable and nothing else could be - so `int32? $x = null;`
        // bound nothing and reached codegen as an untyped null
        //
        // a *class* is no longer on the list on its own, and that is the flip: `Foo $x = null;` leaves this
        // unbound and is reported against the destination, naming `Foo?`
        //
        // asked of AST::bind_null_to, which is the same call AST::CallResolver makes for an argument this
        // position cannot reach: a direct call's parameter types sit on a declaration nobody has chosen
        // yet, so `expected_type` is null there and the binding happens once the callee is known
        if (expected_type != nullptr) {
            AST::bind_null_to(&node, expected_type->type);
        }
        cursor.skip();
        return AST::make_ref(node);
    }

    else if (cursor.is_type(Token::Type::t_string_literal)) {
        auto token = cursor.current();
        auto &node = payload.context.emplace_node<AST::LiteralStringExprNode>(token);

        // decoded here, at the one place a string literal is built, because reporting a bad escape needs
        // the collector and the node has none. the node keeps the *bytes* from this moment on; the token
        // stays verbatim for the code excerpt a diagnostic prints
        if (auto error = AST::decode_string_literal(token.value(), node.decoded_value)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(token), error->message);
        }

        // the type the literal *is*. stamped rather than looked up later, because result_type() has no
        // way to reach the collector - see the field's comment
        if (payload.collector.core_types.has(AST::CoreTypeKind::t_string)) {
            node.core_string_type = payload.collector.core_types.string_type();
        }

        cursor.skip();
        return AST::make_ref(node);
    }

    // `mv E` - take the value out of E. one parse site covers every position a move can appear in
    // (`$b = mv $a`, `consume(mv $a)`, `return mv $a`), because all three arrive here through
    // parse_expr
    //
    // the operand is parsed by recursing into this function rather than by requiring a variable, so
    // `mv $doc->body` reaches AST::OwnershipPass as a real tree and gets the "partial moves are not
    // supported" diagnostic there - where the type is known - instead of an unexpected-token here
    else if (cursor.is_type(Token::Type::t_mv)) {
        auto move_token = cursor.current();
        cursor.skip();

        // the expected type flows through unchanged: `mv` says nothing about what E is, only about
        // who owns it afterwards
        auto operand_ref = parse_expr_node(payload, expected_type);

        if (!operand_ref.has() || !operand_ref.is_expression_node()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(move_token),
                "'mv' expects a value to move out of");
            return AST::make_void_ref();
        }

        auto *operand = operand_ref.unsafe_ptr<AST::ExprNode>();

        // only a place can be moved *out of*: there has to be storage left behind for the move to
        // empty. a non-place is already a move - a call result is nobody else's - so writing `mv` on
        // one is not an error to work around but a sign the author expected a copy to be happening
        if (!AST::is_place_expression(*operand)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(move_token),
                "'mv' needs an expression with storage to move out of - this value is already a temporary, so it moves on its own");
            return AST::make_void_ref();
        }

        auto &move_expr = payload.context.emplace_node<AST::MoveExprNode>(operand, move_token);
        return AST::make_ref(move_expr);
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
        auto found = payload.context.scope().lookup_variable(var_token.value());

        if (!found.decl) {
            payload.collector.collect_issue<AST::Issue::UnknownVariable>(payload.context.code_ref(cursor.current()), cursor.current().value());
            cursor.skip();
            return AST::make_void_ref();
        }

        // the name resolves, but to storage in a frame this one cannot reach. inside a closure that is a
        // *capture*: the value is copied into the closure's environment at the creation site, and the body
        // reads the copy. anywhere else - a plain nested `function`, which has no environment - it is an
        // error, because lowering it would load from an alloca belonging to a different llvm::Function and
        // CodegenContext::var_map would hand over a perfectly valid one
        AST::ExprNode *captured_read = nullptr;

        if (found.crossed_function_boundary()) {
            if (payload.context.current_closure_ptr == nullptr) {
                // a file-scope declaration is one frame out like any other - its storage is a local of
                // the implicit entry point - but saying "an enclosing function" about it names a
                // function the source does not contain, so it gets its own phrasing
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(var_token),
                    found.declared_at_file_scope
                        ? fmt::format(
                            "'{}' is declared at file scope, which is the implicit entry point's own "
                            "frame - a function body cannot reach it. Pass it as a parameter, or write "
                            "a closure.",
                            var_token.value())
                        : fmt::format(
                            "'{}' is declared in an enclosing function. A plain nested function captures "
                            "nothing - pass it as a parameter, or write a closure.",
                            var_token.value()));
                cursor.skip();
                return AST::make_void_ref();
            }

            captured_read =
                Parser::capture_variable(payload, found.decl, var_token, found.boundaries_crossed);

            if (captured_read == nullptr) {
                cursor.skip();
                return AST::make_void_ref();
            }
        }

        auto vardecl = found.decl;

        cursor.skip(); // skip the variable name

        // the base is either the variable itself, or the read of the environment's copy of it
        AST::NodeReference current_ref = AST::make_void_ref();

        if (captured_read != nullptr) {
            current_ref = AST::make_ref(captured_read);
        }
        else {
            auto &varnode = payload.context.emplace_node<AST::VarNode>(vardecl, var_token);
            current_ref = AST::make_ref(payload.context.emplace_node<AST::VarRefNode>(&varnode));
        }
        
        // wrap the base in a MemberAccessNode for each `->member` in the chain
        current_ref = Parser::parse_postfix_chain(payload, current_ref);
        if (!current_ref.has()) {
            return AST::make_void_ref();
        }

        // `$f(...)` - a call through a callable *value*. only a `(` directly after the chain can mean
        // this: a place expression is never followed by one otherwise, so there is nothing to
        // disambiguate against and no need to snapshot
        if (!is_creating_ptr && cursor.is_type(Token::Type::t_open_paren)) {
            auto *callee = current_ref.unsafe_ptr<AST::ExprNode>();

            if (auto *call = Parser::parse_indirect_call(payload, callee, var_token)) {
                return Parser::parse_postfix_chain(payload, AST::make_ref(*call));
            }

            return AST::make_void_ref();
        }

        if (is_creating_ptr) {
            // `&` applies to whatever the postfix chain produced, so `&$s->field` works and is
            // not the assert it used to be - the old code took the address of a VarRefNode it
            // had already replaced with a MemberAccessNode (todo/B4)
            auto *target = current_ref.unsafe_ptr<AST::ExprNode>();
            if (!AST::is_place_expression(*target)) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(var_token),
                    "Cannot take the address of an expression that has no storage");
                return AST::make_void_ref();
            }

            // `&$a[]` borrows the slot an append just grew, to be filled field by field - it does
            // not read what is in it. see AST::IndexExprNode::slot_is_bound, whose other setter is
            // the assignment target in Parser::parse_varexpr
            if (target->get_node_type() == AST::NodeType::n_expr_index) {
                static_cast<AST::IndexExprNode *>(target)->slot_is_bound = true;
            }

            // **the destination decides.** a `weak<T>` slot, parameter, field or return type is asking for
            // a weak reference, and this `&` is how it is spelled; anything else is asking for an address
            // and gets one, whatever the operand's type happens to be. see AddrOfExprNode's header for why
            // it cannot be keyed on the operand instead - the stdlib's generic accessors are the reason
            const bool weak_wanted = expected_type != nullptr && expected_type->type.is_weak();

            auto &ptr_expr = payload.context.emplace_node<AST::AddrOfExprNode>(target, weak_wanted);
            return AST::make_ref(ptr_expr);
        }

        return current_ref;
    }

    // a closure literal, `function(int32 $a) : int32 { ... }`. a value, so it belongs here rather than
    // in the statement dispatch - `starts_funcdecl` is what keeps a *declaration* out of this arm
    else if (Parser::starts_closure_literal(cursor)) {
        auto *closure = Parser::parse_closure_literal(payload);

        if (closure == nullptr) {
            return AST::make_void_ref();
        }

        return AST::make_ref(*closure);
    }

    // `weak($obj)` and `weak<Foo>($obj)` - the written spelling of what `&$obj` on a class already means.
    // both forms build exactly the node the `&` arm builds, so there is one lowering and one ownership
    // rule for the operation however it was written
    //
    // the explicit type argument is accepted and *checked*, not used: the target is whatever the operand
    // already is, so `weak<Bar>($aFoo)` is a mistake worth naming rather than a coercion. it exists so a
    // reader can say what they mean at a site where the operand's type is not obvious
    //
    // unlike `&`, a non-class operand is an error here rather than falling back to a borrow. `&` has to
    // stay total - it is the address-of operator for every type - but nobody writes `weak(...)` meaning
    // "take an address"
    else if (
        cursor.is_type_sequence(0, { Token::Type::t_weak, Token::Type::t_open_paren }) ||
        cursor.is_type_sequence(0, { Token::Type::t_weak, Token::Type::t_open_angle })
    ) {
        return AST::make_ref(Parser::parse_weak_expr(payload));
    }

    // `strong($w)` - the upgrade back, and the only way to read through a weak reference
    else if (cursor.is_type_sequence(0, { Token::Type::t_strong, Token::Type::t_open_paren })) {
        return AST::make_ref(Parser::parse_strong_expr(payload));
    }

    // an explicit pointer cast, `ptr<uint8>($ints:$)` or `int32&($p:$)`. it reinterprets an
    // address as pointing at a different type, so its argument is almost always a `:$`
    // expression (book/concept/pointers_and_refs_v2.md, "Casting")
    //
    // `ptr` always starts one; a plain identifier only does when a `&` and a `(` follow, which
    // no other production spells - `Foo(...)` is a constructor call and `Foo &$x` a declaration
    else if (
        cursor.is_type_sequence(0, { Token::Type::t_ptr, Token::Type::t_open_angle }) ||
        cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_ref, Token::Type::t_open_paren }) ||
        cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_and, Token::Type::t_open_paren })
    ) {
        auto cast_token = cursor.current();

        auto *cast_type = Parser::parse_type(payload);
        if (cast_type == nullptr) {
            return AST::make_void_ref();
        }

        if (!cast_type->type.is_pointer()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(cast_token),
                "Only a pointer type can be written as a cast");
            return AST::make_void_ref();
        }

        auto *inner = parse_parenthesized_operand(payload, cast_type);
        if (inner == nullptr) {
            return AST::make_void_ref();
        }

        auto &cast = payload.context.emplace_node<AST::TypeCastNode>(cast_type->type, inner, false);
        return Parser::parse_postfix_chain(payload, AST::make_ref(cast));
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

    // potential function call - `name(...)` or, with explicit type arguments, `name<...>(...)`
    // a bare identifier is never a comparison operand (values are $-prefixed), so an identifier
    // followed by '<' here is a generic call, not a less-than
    if (
        cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_open_paren }) ||
        cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_open_angle })
    ) {
        auto fcall = parse_funccall(payload, ast_namespace);

        if (fcall == nullptr) {
            return AST::make_void_ref();
        }

        // **and then the suffixes**, which every other operand producer in this function already does.
        // without it a `->` after a free call was `Unexpected token '->'` - not a decision about
        // reading a member off a call result, just the one arm that forgot to continue the chain
        return Parser::parse_postfix_chain(payload, AST::make_ref(*fcall));
    }

    // `&` reached here means it was not followed by a variable name, so there is no storage to
    // take the address of - `&5` and `&get()` are the two ways to spell that. reported with the
    // same message the place check further up uses, rather than falling into the catch-all
    // (book/concept/pointers_and_refs_v2.md, "Taking addresses")
    if (cursor.is_type(Token::Type::t_ref) || cursor.is_type(Token::Type::t_and)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(cursor.current()),
            "Cannot take the address of an expression that has no storage");
        cursor.try_skip_to_next_statement();
        return AST::make_void_ref();
    }

    // nothing in the grammar starts an operand with this token. an assert here aborted the whole
    // compiler with no location and no message the user could act on - and in a release build,
    // where the assert is compiled out, it fell off the end of a function that has to return
    payload.collect_unexpected_token(Token::Type::t_varname);
    cursor.try_skip_to_next_statement();
    return AST::make_void_ref();
}

// parse an operand appearing in prefix position, consuming any leading unary
// '-' / '+' operators and wrapping negations in a UnaryExprNode. unary '+' is
// a no-op and returns the operand unchanged
const AST::NodeReference parse_prefix_unary(Parser::Payload &payload, AST::TypeNode *expected_type)
{
    auto &cursor = payload.cursor;

    // the prefix position: the built-in `-` / `+`, and any symbol a user declared prefix. consumed
    // here rather than in the shunting yard, which has no notion of a one-operand operator - and that
    // placement is also what gives the chapter's "the precedence of unary operators is higher than
    // that of binary operators" for free, since the symbol and its operand are already one operand by
    // the time the yard sees them
    const auto match = operator_match_at(payload, cursor);

    const bool builtin_prefix = match.has()
        && (match.op->type == Token::Type::t_op_sub || match.op->type == Token::Type::t_op_add);
    const bool declared_prefix = match.has() && match.op->has_fixity(AST::OpFixity::t_prefix);

    if (builtin_prefix || declared_prefix) {
        auto op_token = cursor.current();
        cursor.skip(match.token_count);

        // recurse so chained prefixes like `- -$x` and `!!-$x` resolve right to left
        auto operand = parse_prefix_unary(payload, expected_type);
        if (!operand.has()) {
            return AST::make_void_ref();
        }

        auto *operand_expr = operand.unsafe_ptr<AST::ExprNode>();

        // **which meaning applies is decided here, not at the symbol** - it takes the operand's type,
        // and that is not known until the operand has been parsed. so a declared `operator -(Point)`
        // does not stop `-$x` over an int32 from being an ordinary negation
        if (declared_prefix
            && !AST::unary_has_builtin_meaning(match.op, AST::parse_time_operand(operand_expr))) {

            auto *call = Parser::build_operator_call(
                payload, *match.op, AST::OpFixity::t_prefix, op_token, {operand_expr});

            if (call == nullptr) {
                return AST::make_void_ref();
            }

            return Parser::parse_postfix_chain(payload, AST::make_ref(*call));
        }

        // unary plus carries no semantics
        if (match.op->type == Token::Type::t_op_add) {
            return operand;
        }

        auto &unary = payload.context.emplace_node<AST::UnaryExprNode>(op_token, operand_expr);
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

    // an operand, and then whatever suffix operators follow it. this and the yard's own operand arm
    // are the two operand positions in the grammar, so wrapping both is what makes `1m` and
    // `$distance mm` the same rule rather than a literal special case
    return parse_suffix_operator_chain(payload, parse_expr_node(payload, expected_type));
}

// an aggregate, so it is appended with `push_back({...})` rather than `emplace_back(a, b)`: the
// two-argument emplace needs C++20 parenthesized aggregate initialization (P0960), which AppleClang
// does not implement. the braced temporary costs a copy of two words - do not "fix" it back
struct ExprPart
{
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

    for(auto part : expr_parts) {
        // if its a literal, variable etc. (not an operator)
        if (part.opnode == nullptr) {
            output.push_back(part);
        }
        else if (part.opnode->op->type == Token::Type::t_open_paren) {
            operator_stack.push(part.opnode);
        }
        else if (part.opnode->op->type == Token::Type::t_close_paren) {
            while (!operator_stack.empty() && operator_stack.top()->op->type != Token::Type::t_open_paren) {
                output.push_back({AST::make_void_ref(), operator_stack.top()});
                operator_stack.pop();
            }

            // ensure we have the opening "(", otherwise something is off
            assert(operator_stack.top()->op->type == Token::Type::t_open_paren);
            operator_stack.pop();
        }
        else {
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

    while (!operator_stack.empty()) {
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

    while(is_expr_token(payload, cursor)) {
        // if we have a closing parenthesis and the depth is 0, we can break the loop
        // because we have reached the end of the expression
        if (cursor.is_type(Token::Type::t_close_paren) && depth == 0) {
            break;
        }

        // try to parse an operator. a *sequence* lookup, not a single-token one: a declared symbol may
        // be spelled out of several ordinary tokens (`!!`, `<=>`), and the lexer knows nothing about
        // any of them
        const auto match = operator_match_at(payload, cursor);
        const AST::Operator *op = match.op;

        // a '-' or '+' in operand position is a prefix unary operator, not a
        // binary one. detect it and parse the operand it applies to, otherwise
        // the shunting yard would hand parse_binary_expr a null lhs and crash
        bool expects_operand = expr_parts.empty() ||
            (expr_parts.back().opnode != nullptr &&
             expr_parts.back().opnode->op->type != Token::Type::t_close_paren);

        // ...and the same is true of any symbol declared in *prefix* position. without this arm a word
        // operator in operand position - the plain call `avg(1.0, 2.0)`, where `avg` is also declared
        // infix - would be read as an operator with nothing on its left
        if (op != nullptr && expects_operand &&
            (op->type == Token::Type::t_op_sub || op->type == Token::Type::t_op_add
                || op->has_fixity(AST::OpFixity::t_prefix)))
        {
            auto node = parse_prefix_unary(payload, expected_type);
            if (!node.has()) {
                return AST::make_void_ref();
            }
            expr_parts.push_back({node, nullptr});
            continue;
        }

        // **an infix operator, or nothing.** a *custom* symbol declared only in prefix or suffix
        // position is not one, and has to fall through so it is parsed as part of an operand instead:
        // `500mm` reaches here after an operand, and reading a suffix-only `mm` as infix would make
        // the yard pop two operands for it
        //
        // a built-in symbol is always usable here whatever anybody declared, because its infix meaning
        // is the language's - `$a - $b` does not stop being a subtraction because somebody wrote a
        // prefix `-` for a struct
        // ...and a custom symbol in **operand** position is not an infix operator either, whatever it
        // was declared as. that is what keeps a word operator usable as a function name: with `avg`
        // declared infix, the ordinary call `avg(1.0, 2.0)` arrives here at offset 0, where reading it
        // as an operator leaves the yard popping two operands for something with no left operand at
        // all. falling through parses it as the operand it is
        const bool usable_here = op != nullptr
            && (!op->is_custom() || (op->has_fixity(AST::OpFixity::t_infix) && !expects_operand));

        if (usable_here) {
            auto &opnode = payload.context.emplace_node<AST::OperatorNode>(cursor.current(), op);

            cursor.skip(match.token_count);
            expr_parts.push_back({AST::make_void_ref(), &opnode});

            // if the operator is a open parenthesis, we increase the depth
            if (op->type == Token::Type::t_open_paren) {
                depth++;
            } else if (op->type == Token::Type::t_close_paren) {
                depth--;
            }

            continue;
        }

        // **two operands with nothing joining them**, `echo 1 2`. caught here, where the cursor still
        // says where the second one starts - it used to fall through to a `node_stack.size() == 1`
        // assert at the bottom of this function, which takes the compiler down instead of reporting
        //
        // the array literal production widened the ways to arrive here, because a `[` that no postfix
        // chain claimed now parses as one operand rather than being an unexpected token - `5[0]` and
        // `[1, 2][0]` reach it. a *call* no longer does: the chain runs on one, and its bracket arm
        // gives the same message from where the decision is (todo/A13c). so this needs to be a
        // diagnostic before it is anything else
        if (!expr_parts.empty() && expr_parts.back().opnode == nullptr) {
            const bool looks_like_indexing = cursor.is_type(Token::Type::t_open_bracket);

            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(cursor.current()),
                looks_like_indexing
                    ? "only a place can be indexed - a variable, a field or an element. Bind this "
                      "expression to a name first, then index that."
                    : fmt::format(
                        "unexpected '{}' - two expressions with no operator between them.",
                        cursor.current().value()));

            return AST::make_void_ref();
        }

        // parse the next expression node, plus any suffix operators applied to it
        auto node = parse_suffix_operator_chain(payload, parse_expr_node(payload, expected_type));

        // if the node is empty
        if (!node.has()) {
            return AST::make_void_ref();
        }

        expr_parts.push_back({node, nullptr});
    }

    // if we have only one part, we can return it directly
    if (expr_parts.size() == 1) {
        assert(expr_parts[0].opnode == nullptr && "expected no operator");
        return expr_parts[0].node;
    }

    auto postfix_expr = shunting_yard(expr_parts);

    // build expressions nodes
    std::stack<AST::NodeReference> node_stack;
    for (auto &part : postfix_expr) {
        if (part.opnode != nullptr) {
            // a binary operator needs two operands. writing one where a value belongs -
            // `&($a + $b)`, or a stray leading `*` - used to pop an empty stack and take the
            // compiler down with it, so report it as the syntax error it is
            if (node_stack.size() < 2) {
                payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                    payload.context.code_ref(part.opnode->token_literal),
                    Token::Type::t_unknown,
                    part.opnode->token_literal.type()
                );
                return AST::make_void_ref();
            }

            auto right = node_stack.top();
            node_stack.pop();

            auto left = node_stack.top();
            node_stack.pop();

            // let our binary expresssion parser take over
            // there is not much to parse here but it will handle type casts
            // and other node transformation to ensure echos expression behavior
            auto combined = parse_binary_expr(payload, part.opnode, left, right);

            // it can fail now that a declared operator is resolved in there, and a void ref pushed
            // back onto this stack would be the *next* operator's null left operand - dereferenced
            // one iteration later, with nothing left to say where it came from
            if (!combined.has()) {
                return AST::make_void_ref();
            }

            node_stack.push(combined);
        }
        else {
            node_stack.push(part.node);
        }
    }

    // the operand-after-operand check in the collection loop above is what guarantees this, so a
    // mismatch here is a parser bug rather than a bad program. reported rather than asserted all the
    // same: an assert takes the compiler down with no location at all, which is the worst of the
    // three possible outcomes even when the cause really is ours
    if (node_stack.size() != 1) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(token),
            "this expression could not be read as a single value.");
        return AST::make_void_ref();
    }

    return node_stack.top();
}