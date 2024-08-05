#include "Parser/ExprParser.h"

#include "AST/ASTOps.h"
#include "AST/ExprNode.h"
#include "AST/VarRefNode.h"
#include "AST/OperatorNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/TypeCastNode.h"

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
        else if (expected_type->type.is_integer_type()) 
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
        
        // cannot cast
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
    auto current_token = cursor.current();

    // we first check if the literal is larger then a 32bit integer, if so we automatically create a 64bit integer
    InfInt intvalue(current_token.value());
    auto guessed_int_type = AST::ValueTypePrimitive::t_int32;

    if (intvalue > AST::get_integer_size(AST::ValueTypePrimitive::t_int32).get_max_positive_value()) {
        guessed_int_type = AST::ValueTypePrimitive::t_int64;
    }

    auto &node = payload.context.emplace_node<AST::LiteralIntExprNode>(current_token, guessed_int_type);
    cursor.skip();

    // if there is a specified expected type, check if the literal fits the type
    if (expected_type != nullptr) 
    {
        // floats / doubles
        // if the expected type is a float, we can "safely" convert the integer to a float
        if (expected_type->type.is_floating_type()) 
        {
            // we can safely convert the integer to a float
            auto &casted_node = payload.context.emplace_node<AST::LiteralFloatExprNode>(current_token, expected_type->type.get_primitive_type());

            // @TODO we should add a detection if the float value is actually the same as the integer value
            // as very large integers will loose precision when converted to a float
            return AST::make_ref(casted_node);
        }

        // integers
        else if (expected_type->type.is_integer_type())
        {
            auto &expected_node = payload.context.emplace_node<AST::LiteralIntExprNode>(current_token, expected_type->type.get_primitive_type());

            // check if the expected type is unsigned and the literal is negative
            // which should throw an error
            if (expected_type->type.is_unsigned_integer() && intvalue < 0) {
                payload.collector.collect_issue<AST::Issue::InvalidTypeConversion>(
                    payload.context.code_ref(current_token), 
                    std::format(
                        "The integer literal '{}' cannot be implicitly converted to an unsigned integer because it is negative.", 
                        current_token.value()
                    )
                );

                return AST::make_void_ref();
            }

            // check if the literal fits the expected type
            auto int_size = AST::get_integer_size(expected_type->type.get_primitive_type());
            auto lower_bound = int_size.get_max_negative_value();
            auto upper_bound = int_size.get_max_positive_value();

            if (intvalue < lower_bound) {
                payload.collector.collect_issue<AST::Issue::IntegerUnderflow>(
                    payload.context.code_ref(current_token), 
                    std::format(
                        "The literal '{}' is too small for the integer type '{}'. The minimum value is '{}'.", 
                        current_token.value(),
                        AST::get_primitive_name(expected_type->type.get_primitive_type()),
                        lower_bound
                    )
                );

                return AST::make_void_ref();
            }

            if (intvalue > upper_bound) {
                payload.collector.collect_issue<AST::Issue::IntegerOverflow>(
                    payload.context.code_ref(current_token), 
                    std::format(
                        "The literal '{}' is too large for the integer type '{}'. The maximum value is '{}'.", 
                        current_token.value(),
                        AST::get_primitive_name(expected_type->type.get_primitive_type()),
                        upper_bound
                    )
                );

                return AST::make_void_ref();
            }

            // if we end up here, the literal fits the expected type and can be used as expected
            return AST::make_ref(expected_node);
        }

        // cannot cast
        else {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                payload.context.code_ref(current_token), 
                Token::Type::t_unknown,
                current_token.type()
            );
        }
    }

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

AST::ExprNode *try_implicit_cast(Parser::Payload &payload, AST::ExprNode *source, const AST::TypeNode *expected_type)
{   
    // if the types match we can return the source node directly
    if (source->result_type() == expected_type->type) {
        return source;
    }

    
}

const AST::NodeReference parse_expr_node(Parser::Payload &payload, AST::TypeNode *expected_type)
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

    if (cursor.is_type(Token::Type::t_string_literal)) {
        auto &node = payload.context.emplace_node<AST::LiteralStringExprNode>(cursor.current());
        cursor.skip();
        return AST::make_ref(node);
    }

    if (
        cursor.is_type(Token::Type::t_varname) || 
        cursor.is_type_sequence(0, { Token::Type::t_ref, Token::Type::t_varname })
    ) {
        // if the token is a reference operator we have to handle it
        bool is_creating_ptr = cursor.is_type(Token::Type::t_ref);
        if (is_creating_ptr) {
            cursor.skip();
        }

        auto vardecl = payload.context.scope().find_vardecl_by_name(cursor.current().value());

        if (!vardecl) {
            payload.collector.collect_issue<AST::Issue::UnknownVariable>(payload.context.code_ref(cursor.current()), cursor.current().value());
            cursor.skip();
            return AST::make_void_ref();
        }

        auto &varref = payload.context.emplace_node<AST::VarRefNode>(cursor.current(), vardecl);

        if (!is_creating_ptr) {
            auto &node = payload.context.emplace_node<AST::VarRefExprNode>(&varref);

            cursor.skip();

            // if there is a expected type, we have to check if it matches the variable type
            if (expected_type != nullptr) {
                if (vardecl->type_node()->type != expected_type->type) {
                    
                    // create a cast node and return it
                    auto &cast_node = payload.context.emplace_node<AST::TypeCastNode>(expected_type->type, &node, true);

                    // check if the cast could cause a loss of precision
                    auto source_size = AST::get_integer_size(vardecl->type_node()->type.get_primitive_type());
                    auto target_size = AST::get_integer_size(expected_type->type.get_primitive_type());

                    if (source_size.size > target_size.size) {
                        payload.collector.collect_issue<AST::Issue::LossOfPrecision>(
                            payload.context.code_ref(varref.token_varname), 
                            std::format(
                                "The variable '{}' is casted from '{}' to '{}' which will result in a loss of precision.", 
                                vardecl->name(),
                                AST::get_primitive_name(vardecl->type_node()->type.get_primitive_type()),
                                AST::get_primitive_name(expected_type->type.get_primitive_type())
                            )
                        );
                    }

                    return AST::make_ref(cast_node);
                }
            }
            
            return AST::make_ref(node);
        }
        else 
        {
            auto &node = payload.context.emplace_node<AST::VarPtrExprNode>(&varref);
            cursor.skip();
            return AST::make_ref(node);
        }
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

    // potential function call
    if (cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_open_paren })) {

        // check if the identifier token is a scalar type, and we simply generate a cast node
        // if (payload.cursor.current().value())

        auto fcall = parse_funccall(payload, ast_namespace);
        return AST::make_ref(fcall);
    }

    assert(false && "unimplemented");
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

            auto &node = payload.context.emplace_node<AST::BinaryExprNode>(
                part.opnode, 
                left.unsafe_ptr<AST::ExprNode>(), 
                right.unsafe_ptr<AST::ExprNode>()
            );
            node_stack.push(AST::make_ref(node));
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