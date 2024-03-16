#ifndef TOKEN_H
#define TOKEN_H

#pragma once

#include <vector>
#include <string>

struct Token {
public:
    enum class Type {
        t_identifier, 
        t_semicolon,                // ;
        t_colon,                    // :
        t_comma,                    // ,
        t_dot,                      // .
        t_logical_and,              // &&
        t_logical_or,               // ||
        t_logical_eq,               // ==
        t_logical_neq,              // !=
        t_logical_leq,              // <=
        t_logical_geq,              // >=
        t_accessorlr,               // ->
        t_assign,                   // =
        t_op_inc,                   // ++
        t_op_dec,                   // --
        t_op_add,                   // +       
        t_op_sub,                   // -
        t_op_mul,                   // *
        t_op_div,                   // /
        t_op_mod,                   // %
        t_qmark,                    // ?
        t_exclamation,              // !
        t_open_angle,               // <
        t_close_angle,              // >
        t_open_paren,               // (
        t_close_paren,              // )
        t_open_brace,               // {
        t_close_brace,              // }
        t_open_bracket,             // [
        t_close_bracket,            // ]
        t_string_literal,           // "..."
        t_integer_literal,          // 123
        t_hex_literal,              // 0x123
        t_binary_literal,           // 0b101
        t_floating_literal,         // 123.456
        t_bool_literal,             // true, false
        t_varname,                  // $varname
        t_unknown
    };

    Type type;
    size_t line;
    size_t char_offset;

    Token(Type type, size_t line, size_t char_offset)
        : type(type), line(line), char_offset(char_offset) {}
};

struct TokenReference;
struct TokenCollection {
    std::vector<Token> tokens;
    std::vector<std::string> token_values;

    void push(const std::string &value, Token::Type type, size_t line, size_t char_offset) {
        tokens.emplace_back(type, line, char_offset);
        token_values.push_back(value);
    }

    void clear() {
        tokens.clear();
        token_values.clear();
    }

    inline size_t size() const {
        return tokens.size();
    }

    TokenReference operator[](size_t index) const;
};

class TokenReference {
    const TokenCollection &tokens;
    size_t index;

public:

    TokenReference(const TokenCollection &tokens, size_t index)
        : tokens(tokens), index(index) 
    {}

    TokenReference(TokenCollection *tokens, size_t index)
        : tokens(*tokens), index(index) 
    {}

    inline const bool is_valid() const {
        return index < tokens.tokens.size();
    }

    inline const Token::Type type() const {
        return tokens.tokens[index].type;
    }

    inline const std::string &value() const {
        return tokens.token_values[index];
    }
};


#endif