#include "Token.h"

TokenReference TokenCollection::operator[](size_t index) const
{
    return TokenReference(*this, index);
}

TokenSlice TokenCollection::slice(size_t start, size_t end) const
{
    return TokenSlice(*this, start, end);
}

const TokenReference TokenSlice::start_ref() const
{
    return TokenReference(tokens, start_index);
}

const TokenReference TokenSlice::end_ref() const
{
    return TokenReference(tokens, end_index);
}

TokenSlice TokenReference::make_slice(size_t offset) const
{
    return tokens.slice(index, index + offset);
}

const std::string token_type_string(Token::Type type)
{
    switch (type) {
        case Token::Type::t_identifier: return "identifier";
        case Token::Type::t_semicolon: return "semicolon (;)";
        case Token::Type::t_colon: return "colon (:)";
        case Token::Type::t_comma: return "comma (,)";
        case Token::Type::t_dot: return "dot (.)";
        case Token::Type::t_logical_and: return "logical_and (&&)";
        case Token::Type::t_logical_or: return "logical_or (||)";
        case Token::Type::t_logical_eq: return "logical_eq (==)";
        case Token::Type::t_logical_neq: return "logical_neq (!=)";
        case Token::Type::t_logical_leq: return "logical_leq (<=)";
        case Token::Type::t_logical_geq: return "logical_geq (>=)";
        case Token::Type::t_accessorlr: return "accessorlr (->)";
        case Token::Type::t_assign: return "assign (=)";
        case Token::Type::t_op_inc: return "op_inc (++)";
        case Token::Type::t_op_dec: return "op_dec (--)";
        case Token::Type::t_op_add: return "op_add (+)";
        case Token::Type::t_op_sub: return "op_sub (-)";
        case Token::Type::t_op_mul: return "op_mul (*)";
        case Token::Type::t_op_div: return "op_div (/)";
        case Token::Type::t_op_mod: return "op_mod (%)";
        case Token::Type::t_qmark: return "qmark (?)";
        case Token::Type::t_exclamation: return "exclamation (!)";
        case Token::Type::t_open_angle: return "open_angle (<)";
        case Token::Type::t_close_angle: return "close_angle (>)";
        case Token::Type::t_open_paren: return "open_paren '('";
        case Token::Type::t_close_paren: return "close_paren ')'";
        case Token::Type::t_open_brace: return "open_brace '{'";
        case Token::Type::t_close_brace: return "close_brace '}'";
        case Token::Type::t_open_bracket: return "open_bracket '['";
        case Token::Type::t_close_bracket: return "close_bracket ']'";
        case Token::Type::t_string_literal: return "string_literal (\"\")";
        case Token::Type::t_integer_literal: return "integer_literal";
        case Token::Type::t_hex_literal: return "hex_literal";
        case Token::Type::t_binary_literal: return "binary_literal";
        case Token::Type::t_floating_literal: return "floating_literal";
        case Token::Type::t_bool_literal: return "bool_literal";
        case Token::Type::t_varname: return "varname";
        case Token::Type::t_const: return "const";
        case Token::Type::t_unknown: return "unknown";
        default: return "[undefined]";
    }
}

bool Token::is_operator_type() const
{
    return is_one_of({
        Token::Type::t_assign,
        Token::Type::t_logical_or,
        Token::Type::t_logical_and,
        Token::Type::t_logical_eq,
        Token::Type::t_logical_neq,
        Token::Type::t_open_angle,
        Token::Type::t_close_angle,
        Token::Type::t_logical_geq,
        Token::Type::t_logical_leq,
        Token::Type::t_op_add,
        Token::Type::t_op_sub,
        Token::Type::t_op_mul,
        Token::Type::t_op_div,
        Token::Type::t_op_mod,
        Token::Type::t_op_pow,
        Token::Type::t_op_inc,
        Token::Type::t_op_dec
    });
}
