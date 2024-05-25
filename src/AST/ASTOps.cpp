#include "AST/ASTOps.h"

#include <stack>
#include <iostream>

AST::OpPrecedence AST::Operator::get_precedence(const Token::Type &type)
{
    switch (type)
    {
        case Token::Type::t_assign:
            return {OpAssociativity::right, 1};
        case Token::Type::t_logical_or:
            return {OpAssociativity::left, 2};
        case Token::Type::t_logical_and:
            return {OpAssociativity::left, 3};
        case Token::Type::t_logical_eq:
        case Token::Type::t_logical_neq:
            return {OpAssociativity::left, 4};
        case Token::Type::t_open_angle:
        case Token::Type::t_close_angle:
        case Token::Type::t_logical_geq:
        case Token::Type::t_logical_leq:
            return {OpAssociativity::left, 5};
        case Token::Type::t_op_add:
        case Token::Type::t_op_sub:
            return {OpAssociativity::left, 6};
        case Token::Type::t_op_mul:
        case Token::Type::t_op_div:
            return {OpAssociativity::left, 7};
        case Token::Type::t_op_mod:
            return {OpAssociativity::left, 8};
        case Token::Type::t_op_pow:
            return {OpAssociativity::right, 9};
        case Token::Type::t_op_inc:
        case Token::Type::t_op_dec:
            return {OpAssociativity::right, 20};
        case Token::Type::t_open_paren:
            return {OpAssociativity::none, 30};
        default:
            return {OpAssociativity::none, 0};
    };
}

#define O1Prec get_precedence(token.type())
#define O2Prec get_precedence(tokens[operator_stack.top()].type())

TokenList AST::Operator::shunting_yard(TokenSlice &token_slice)
{
    auto &tokens = token_slice.tokens;

    auto output = TokenList(tokens);
    auto operator_stack = std::stack<size_t>();

    for(auto token : token_slice)
    {
        auto &tokenval = token.value();

        if (token.token().is_one_of({
            Token::Type::t_string_literal,
            Token::Type::t_integer_literal,
            Token::Type::t_hex_literal,
            Token::Type::t_binary_literal,
            Token::Type::t_floating_literal,
            Token::Type::t_bool_literal,
            Token::Type::t_varname
        })) {
            output.push(token);   
        }
        else if (token.token().is_operator_type())
        {
            while (
                !operator_stack.empty() &&
                O2Prec.assoc == OpAssociativity::right &&
                (
                    O2Prec.precedence > O1Prec.precedence ||
                    (
                        O1Prec.precedence == O2Prec.precedence &&
                        O1Prec.assoc == OpAssociativity::left
                    )
                )
            ) {
                output.push(tokens[operator_stack.top()]);
                operator_stack.pop();
            }

            operator_stack.push(token.get_handle());
        }
        else if(token.type() == Token::Type::t_open_paren)
        {
            operator_stack.push(token.get_handle());
        }
        else if(token.type() == Token::Type::t_close_paren)
        {
            while(!operator_stack.empty() && tokens[operator_stack.top()].type() != Token::Type::t_open_paren)
            {
                output.push(tokens[operator_stack.top()]);
                operator_stack.pop();
            }

            // ensure we have the opening "(", otherwise something is off
            assert(tokens[operator_stack.top()].token().is_a(Token::Type::t_open_paren));
            operator_stack.pop();
        }
        else {
            throw std::runtime_error("Unknown token type");
        }
    }

    while (!operator_stack.empty())
    {
        output.push(tokens[operator_stack.top()]);
        operator_stack.pop();
    }

    return output;
}