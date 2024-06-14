#include "AST/ASTOps.h"

#include <stack>
#include <iostream>
#include <memory>

AST::OpPrecedence AST::Operator::get_precedence_for_token(const Token::Type &type)
{
    switch (type)
    {
        // parantheses
        case Token::Type::t_open_paren:
        case Token::Type::t_close_paren:
            return {OpAssociativity::none, 1};
        
        // increment/decrement
        case Token::Type::t_op_inc:
        case Token::Type::t_op_dec:
            return {OpAssociativity::right, 2};
        
        // exponent
        case Token::Type::t_op_pow:
            return {OpAssociativity::right, 3};
        
        // multiplication, division, modulo
        case Token::Type::t_op_mul:
        case Token::Type::t_op_div:
        case Token::Type::t_op_mod:
            return {OpAssociativity::left, 4};

        // addition, subtraction
        case Token::Type::t_op_add:
        case Token::Type::t_op_sub:
            return {OpAssociativity::left, 5};

        // bitwise shift
        case Token::Type::t_op_shl:
        case Token::Type::t_op_shr:
            return {OpAssociativity::left, 6};

        // and, xor, or
        case Token::Type::t_and:
            return {OpAssociativity::left, 7};
        case Token::Type::t_xor:
            return {OpAssociativity::left, 8};
        case Token::Type::t_or:
            return {OpAssociativity::left, 9};

        // comparison
        case Token::Type::t_open_angle:
        case Token::Type::t_close_angle:
        case Token::Type::t_logical_geq:
        case Token::Type::t_logical_leq:
        case Token::Type::t_logical_eq:
        case Token::Type::t_logical_neq:
            return {OpAssociativity::left, 10};

        case Token::Type::t_logical_and:
            return {OpAssociativity::left, 11};

        case Token::Type::t_logical_or:
            return {OpAssociativity::left, 12};

        // assignment
        case Token::Type::t_assign:
            return {OpAssociativity::right, 13};

        default:
            return {OpAssociativity::none, 0};
    };
}

// #define O1Prec get_precedence_for_token(token.type())
// #define O2Prec get_precedence_for_token(tokens[operator_stack.top()].type())

// TokenList AST::Operator::shunting_yard(TokenSlice &token_slice)
// {
//     auto &tokens = token_slice.tokens;

//     auto output = TokenList(tokens);
//     auto operator_stack = std::stack<size_t>();

//     for(auto token : token_slice)
//     {
//         // auto &tokenval = token.value();

//         if (token.token().is_one_of({
//             Token::Type::t_string_literal,
//             Token::Type::t_integer_literal,
//             Token::Type::t_hex_literal,
//             Token::Type::t_binary_literal,
//             Token::Type::t_floating_literal,
//             Token::Type::t_bool_literal,
//             Token::Type::t_varname
//         })) {
//             output.push(token);   
//         }
//         else if (token.token().is_operator_type())
//         {
//             while (
//                 !operator_stack.empty() &&
//                 O2Prec.assoc == OpAssociativity::right &&
//                 (
//                     O2Prec.sequence > O1Prec.sequence ||
//                     (
//                         O1Prec.sequence == O2Prec.sequence &&
//                         O1Prec.assoc == OpAssociativity::left
//                     )
//                 )
//             ) {
//                 output.push(tokens[operator_stack.top()]);
//                 operator_stack.pop();
//             }

//             operator_stack.push(token.get_handle());
//         }
//         else if(token.type() == Token::Type::t_open_paren)
//         {
//             operator_stack.push(token.get_handle());
//         }
//         else if(token.type() == Token::Type::t_close_paren)
//         {
//             while(!operator_stack.empty() && tokens[operator_stack.top()].type() != Token::Type::t_open_paren)
//             {
//                 output.push(tokens[operator_stack.top()]);
//                 operator_stack.pop();
//             }

//             // ensure we have the opening "(", otherwise something is off
//             assert(tokens[operator_stack.top()].token().is_a(Token::Type::t_open_paren));
//             operator_stack.pop();
//         }
//         else {
//             throw std::runtime_error("Unknown token type");
//         }
//     }

//     while (!operator_stack.empty())
//     {
//         output.push(tokens[operator_stack.top()]);
//         operator_stack.pop();
//     }

//     return output;
// }

AST::OperatorRegistry::OperatorRegistry()
{
    _predefined_operator_map.fill(nullptr);

    // register the predefined operators
    register_predefined_token_op(Token::Type::t_assign);
    register_predefined_token_op(Token::Type::t_and);
    register_predefined_token_op(Token::Type::t_or);
    register_predefined_token_op(Token::Type::t_xor);
    register_predefined_token_op(Token::Type::t_open_paren);
    register_predefined_token_op(Token::Type::t_close_paren);
    register_predefined_token_op(Token::Type::t_logical_or);
    register_predefined_token_op(Token::Type::t_logical_and);
    register_predefined_token_op(Token::Type::t_logical_eq);
    register_predefined_token_op(Token::Type::t_logical_neq);
    register_predefined_token_op(Token::Type::t_open_angle);
    register_predefined_token_op(Token::Type::t_close_angle);
    register_predefined_token_op(Token::Type::t_logical_geq);
    register_predefined_token_op(Token::Type::t_logical_leq);
    register_predefined_token_op(Token::Type::t_op_shl);
    register_predefined_token_op(Token::Type::t_op_shr);
    register_predefined_token_op(Token::Type::t_op_add);
    register_predefined_token_op(Token::Type::t_op_sub);
    register_predefined_token_op(Token::Type::t_op_mul);
    register_predefined_token_op(Token::Type::t_op_div);
    register_predefined_token_op(Token::Type::t_op_mod);
    register_predefined_token_op(Token::Type::t_op_pow);
    register_predefined_token_op(Token::Type::t_op_inc);
    register_predefined_token_op(Token::Type::t_op_dec);
}

void AST::OperatorRegistry::register_predefined_token_op(const Token::Type &type)
{
    auto t = Token(type, 0, 0);
    assert(t.is_operator_type() && "Token type is not an operator"); // sanity check

    auto op = std::make_unique<PredefinedTokenOperator>(type);
    _predefined_operator_map[static_cast<size_t>(type)] = op.get();
    _operator_symbol_map[token_lit_symbol_string(type)] = op.get(); // also store it as a custom operator for easy lookup
    _operators.push_back(std::move(op));
}

void AST::OperatorRegistry::register_custom_op(const std::string &name, int precedence, OpAssociativity assoc)
{
    auto op = std::make_unique<CustomOperator>(name, OpPrecedence{assoc, precedence});
    _operator_symbol_map[name] = op.get();
    _custom_operators.push_back(op.get());
    _operators.push_back(std::move(op));
}

const AST::Operator *AST::OperatorRegistry::get_operator(const TokenReference &token) const
{
    if (!token.is_valid()) {
        return nullptr;
    }

    // if the token type is an operator, we can use the predefined operator map directly
    if (token.token().is_operator_type()) {
        return _predefined_operator_map[static_cast<size_t>(token.type())];
    }

    // try to match the token value to a custom operator
    auto custom_op = _operator_symbol_map.find(token.value());
    if (custom_op != _operator_symbol_map.end()) {
        return custom_op->second;
    }

    return nullptr;
}

const AST::Operator *AST::OperatorRegistry::get_operator(const std::string &symbol) const
{
    auto custom_op = _operator_symbol_map.find(symbol);
    if (custom_op != _operator_symbol_map.end()) {
        return custom_op->second;
    }

    return nullptr;
}