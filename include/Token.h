#ifndef TOKEN_H
#define TOKEN_H

#pragma once

#include <vector>
#include <string>
#include <cassert>

#include <cstdint>

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
        t_const,                    // const
        t_echo,                     // echo
        t_unknown
    };

    Type type;
    uint32_t line;
    uint32_t char_offset; // if you have a file source file thats 2GB, you're have other problems

    Token(Type type, uint32_t line, uint32_t char_offset)
        : type(type), line(line), char_offset(char_offset) {}
};

// function to convert token type to string
// mostly used for debugging purposes
const std::string token_type_string(Token::Type type);

struct TokenReference;
struct TokenCollection {
    struct Slice {
        const TokenCollection &tokens;
        const size_t start;
        const size_t end;

        const TokenReference start_ref() const;
        const TokenReference end_ref() const;

        const Token &startt() const {
            return tokens.tokens[start];
        }

        const Token &endt() const {
            return tokens.tokens[end];
        }
    };

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

    Slice slice(size_t start, size_t end) const {
        return Slice{*this, start, end};
    }
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

    inline bool belongs_to(const TokenCollection &tokens) const {
        return &this->tokens == &tokens;
    }

    inline const bool is_valid() const {
        return index < tokens.tokens.size();
    }

    inline const std::string &value() const {
        assert(is_valid());
        return tokens.token_values[index];
    }

    inline const Token &token() const {
        assert(is_valid());
        return tokens.tokens[index];
    }

    inline const Token::Type type() const {
        assert(is_valid());
        return tokens.tokens[index].type;
    }

    inline const uint32_t line() const {
        assert(is_valid());
        return tokens.tokens[index].line;
    }

    inline const uint32_t char_offset() const {
        assert(is_valid());
        return tokens.tokens[index].char_offset;
    }

    // same as char_offset..
    inline const uint32_t column() const {
        assert(is_valid());
        return tokens.tokens[index].char_offset;
    }

    inline TokenReference next() const {
        return TokenReference(tokens, index + 1);
    }

    inline TokenReference prev() const {
        return TokenReference(tokens, index - 1);
    }

    inline TokenCollection::Slice make_slice(size_t offset = 0) const {
        return tokens.slice(index, index + offset);
    }
};


#endif