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
        t_and,                      // &
        t_or,                       // |
        t_xor,                      // ^
        t_op_shl,                   // <<
        t_op_shr,                   // >>
        t_op_inc,                   // ++
        t_op_dec,                   // --
        t_op_add,                   // +       
        t_op_sub,                   // -
        t_op_mul,                   // *
        t_op_div,                   // /
        t_op_mod,                   // %
        t_op_pow,                   // **
        t_op_custom,                // <custom>
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
        t_function,                 // function
        t_return,                   // return
        t_if,                       // if
        t_else,                     // else
        t_while,                    // while
        t_for,                      // for
        t_break,                    // break
        t_unknown
    };

    Type type;
    uint32_t line;
    uint32_t char_offset; // if you have a file source file thats 2GB, you're have other problems

    Token(Type type, uint32_t line, uint32_t char_offset)
        : type(type), line(line), char_offset(char_offset) {}

    inline bool is_a(Type type) const {
        return this->type == type;
    }

    inline bool is_one_of(std::initializer_list<Type> types) const {
        for (auto type : types) {
            if (this->type == type) {
                return true;
            }
        }
        return false;
    }

    bool is_operator_type() const;
};

// function to convert token type to string
// mostly used for debugging purposes
const std::string token_type_string(Token::Type type);

// returns a string representation of the literal symbol 
// used for lexing and parsing, only available for literals
const std::string token_lit_symbol_string(const Token::Type type);

class TokenReference;
struct TokenSlice;
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

    TokenSlice slice(size_t start, size_t end) const;
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

    // returns the handle to the token inside of its collection
    // Keep in mind that token and its value is owned by the collection
    // use carefully.
    inline size_t get_handle() const {
        return index;
    }

    inline const TokenCollection &get_collection_ref() const {
        return tokens;
    }

    inline bool is_valid() const {
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

    inline Token::Type type() const {
        assert(is_valid());
        return tokens.tokens[index].type;
    }

    inline uint32_t line() const {
        assert(is_valid());
        return tokens.tokens[index].line;
    }

    inline uint32_t char_offset() const {
        assert(is_valid());
        return tokens.tokens[index].char_offset;
    }

    // same as char_offset..
    inline uint32_t column() const {
        assert(is_valid());
        return tokens.tokens[index].char_offset;
    }

    inline TokenReference next() const {
        return TokenReference(tokens, index + 1);
    }

    inline TokenReference prev() const {
        return TokenReference(tokens, index - 1);
    }

    TokenSlice make_slice(size_t offset = 0) const;

    inline bool operator==(const TokenReference &other) const {
        return &tokens == &other.tokens && index == other.index;
    }

    inline bool operator!=(const TokenReference &other) const {
        return !(*this == other);
    }
};

struct TokenSlice {
    const TokenCollection &tokens;
    const size_t start_index;
    const size_t end_index;

    const TokenReference start_ref() const;
    const TokenReference end_ref() const;

    const Token &startt() const {
        return tokens.tokens[start_index];
    }

    const Token &endt() const {
        return tokens.tokens[end_index];
    }

    // returns the N'th token inside of the slice 
    TokenReference operator[](size_t index) const {
        assert(start_index + index <= end_index);
        return TokenReference(tokens, start_index + index);
    }

    struct iterator {
        const TokenSlice &slice;
        size_t index;

        iterator(const TokenSlice &slice, size_t index)
            : slice(slice), index(index) {}

        TokenReference operator*() const {
            return TokenReference(slice.tokens, index);
        }

        iterator &operator++() {
            index++;
            return *this;
        }

        bool operator!=(const iterator &other) const {
            return index != other.index;
        }
    };

    iterator begin() const {
        return iterator(*this, start_index);
    }

    iterator end() const {
        return iterator(*this, end_index);
    }
};

struct TokenList {
    const TokenCollection &tokens;
    std::vector<size_t> indices;

    TokenList(const TokenCollection &tokens)
        : tokens(tokens) {}

    TokenReference operator[](size_t index) const {
        return TokenReference(tokens, indices[index]);
    }

    void push(size_t index) {
        indices.push_back(index);
    }

    void push(const TokenReference &ref) {
        assert(ref.belongs_to(tokens));
        indices.push_back(ref.get_handle());
    }

    // iterator for the token list
    struct iterator {
        const TokenList &list;
        size_t index;

        iterator(const TokenList &list, size_t index)
            : list(list), index(index) {}

        TokenReference operator*() const {
            return TokenReference(list.tokens, list.indices[index]);
        }

        iterator &operator++() {
            index++;
            return *this;
        }

        bool operator!=(const iterator &other) const {
            return index != other.index;
        }
    };

    iterator begin() const {
        return iterator(*this, 0);
    }

    iterator end() const {
        return iterator(*this, indices.size());
    }
};

#endif