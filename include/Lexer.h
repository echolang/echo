#ifndef LEXER_H
#define LEXER_H

#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <regex>
#include <concepts>
#include <functional>
#include <cstring>

#include "Token.h"

#define MHP_VOCAB_LB '\n'
#define MHP_VOCAB_SPACE ' '
#define MHP_VOCAB_TAB '\t'
#define MHP_VOCAB_DUBQUOTE '"'
#define MHP_VOCAB_SNGQUOTE '\''

struct LexerRule {
    std::regex pattern;
    Token::Type type;
};

struct LexerCursor
{
    size_t line;
    size_t char_offset;
    size_t end_of_line_offset;
    std::string::const_iterator it;
    const std::string &input;

    LexerCursor(const std::string &input) : 
        line(1), char_offset(1), it(input.begin()), input(input) 
    {
        determine_end_of_line();
    }

    inline const std::string::const_iterator begin() const {
        return input.begin();
    }

    inline const std::string::const_iterator end() const {
        return input.end();
    }

    inline const std::string::const_iterator end_of_line() const {
        return input.begin() + end_of_line_offset;
    }

    inline bool is_eof() const {
        return it == input.end();
    }

    inline char peek() const {
        return *it;
    }

    inline char peek(size_t offset) const {
        return *(it + offset);
    }

    inline void determine_end_of_line() {
        if (is_eof()) {
            end_of_line_offset = input.size();
        } else {
            end_of_line_offset = input.find(MHP_VOCAB_LB, it - input.begin());
            end_of_line_offset = end_of_line_offset > input.size() ? input.size() : end_of_line_offset;
        }
    }

    inline void skip(size_t offset = 1) {
        bool did_increment_line = false;
        if (peek() == MHP_VOCAB_LB) {
            line++;
            char_offset = 1;
            did_increment_line = true;
        } else {
            char_offset += offset;
        }

        it += offset;

        if (did_increment_line) {
            determine_end_of_line();
        }
    }

    inline void skip_formatting() {
        while (is_formatting() && !is_eof()) {
            skip();
        }
    }

    inline void skip_until(char c) {
        while (peek() != c && !is_eof()) {
            skip();
        }
    }

    inline void advance() {
        if (is_eof()) {
            return;
        }

        // we do not tokenize any formatting characters
        // so advance the cursor until we find a non-formatting character
        skip_formatting();

        if (!is_eof()) {
            skip();
        }
    }

    // returns true if the input at the current cursor position
    // matches the given string
    inline bool begins_with(const std::string &str) {
        return input.compare(it - input.begin(), str.size(), str) == 0;
    }

    inline bool begins_with(const char *str) {
        return input.compare(it - input.begin(), strlen(str), str) == 0;
    }

    inline bool is_quote() {
        return peek() == MHP_VOCAB_DUBQUOTE || peek() == MHP_VOCAB_SNGQUOTE;
    }

    inline bool is_formatting() {
        return peek() == MHP_VOCAB_SPACE || peek() == MHP_VOCAB_TAB || peek() == MHP_VOCAB_LB;
    }
};


class Lexer 
{
    template<size_t N>
    struct CSXStrLiteral {
        constexpr CSXStrLiteral(const char (&str)[N]) {
            std::copy_n(str, N, value);
        }
        char value[N];
    };

    using LexerFunctionSignature = std::function<bool(Lexer&, TokenCollection&, LexerCursor&)>;

    struct LexerFunctionEntry {
        void *context;
        LexerFunctionSignature function;
    };

public:
    struct TokenException : public std::exception {

        const std::string snippet;
        const size_t line;
        const size_t char_offset;
        const std::string error_message;

        TokenException(const std::string &message, const std::string &snippet, size_t line, size_t char_offset) :
            snippet(snippet), 
            line(line), 
            char_offset(char_offset),
            error_message(message)
        {}

        const char *what() const throw() {
            return error_message.c_str();
        }
    };

    struct UnknownTokenException : public TokenException {
        UnknownTokenException(const std::string &snippet, size_t line, size_t char_offset) :
            TokenException("Unknown token at line " + std::to_string(line) + " offset " + std::to_string(char_offset) + " near: " + snippet, snippet, line, char_offset)
        {}
    };

    struct UnterminatedStringException : public TokenException {
        UnterminatedStringException(const std::string &snippet, size_t line, size_t char_offset) :
            TokenException("Unterminated string at line " + std::to_string(line) + " offset " + std::to_string(char_offset) + " near: " + snippet, snippet, line, char_offset)
        {}
    };

    // struct RegexTokenMatcher {
    //     std::regex pattern;
    //     Token::Type type;

    //     RegexTokenMatcher(const std::string &pattern, Token::Type type) : pattern(pattern), type(type) {}

    //     bool match(TokenCollection &tokens, LexerCursor &cursor) {
    //         std::smatch match;
    //         if (!std::regex_search(cursor.it, cursor.input.end(), match, pattern)) {
    //             return false;
    //         }

    //         tokens.push(match.str(), type, cursor.line, cursor.char_offset);
    //         cursor.skip(match.str().size());
    //         return true;
    //     }
    // };

    Lexer() {}

    /**
     * Returns a single character parser function, useful for (<, >, ?, etc.)
     */
    template <char C, Token::Type T>
    bool parse_char_token(TokenCollection &tokens, LexerCursor &cursor) {
        if (cursor.peek() != C) {
            return false;
        }

        tokens.push(std::string(1, C), T, cursor.line, cursor.char_offset);
        cursor.skip();
        return true;
    }

    /**
     * Parses a multi-character token (e.g. "==" or "!=")
     */
    template <CSXStrLiteral lit, Token::Type T>
    bool parse_exact_token(TokenCollection &tokens, LexerCursor &cursor) {
        constexpr auto size = sizeof(lit.value) - 1;
        constexpr auto contents = lit.value;

        if (!cursor.begins_with(contents)) {
            return false;
        }

        tokens.push(std::string(contents, size), T, cursor.line, cursor.char_offset);
        cursor.skip(size);
        return true;
    }

    /**
     * Parses a token with the given regex pattern
     */
    template <CSXStrLiteral pattern, Token::Type T>
    bool parse_regex_token(TokenCollection &tokens, LexerCursor &cursor) {
        constexpr auto size = sizeof(pattern.value);
        constexpr auto contents = pattern.value;

        static std::regex regex(contents, std::regex_constants::optimize);

        std::smatch match;
        if (!std::regex_search(cursor.it, cursor.end_of_line(), match, regex)) {
            return false;
        }

        auto match_str = match.str();

        tokens.push(match_str, T, cursor.line, cursor.char_offset);
        cursor.skip(match_str.size());
        return true;
    }

    /**
     * Parse variable names
     */
    bool parse_varname(TokenCollection &tokens, LexerCursor &cursor);

    /**
     * Parses a string literal
     */
    bool parse_string_literal(TokenCollection &tokens, LexerCursor &cursor);

    /**
     * Parses a hex literal
     */
    bool parse_hex_literal(TokenCollection &tokens, LexerCursor &cursor);

    /**
     * Parses a single line comment
     */
    bool parse_sl_comment(TokenCollection &tokens, LexerCursor &cursor);

    /**
     * Parses a multi-line comment
     */
    bool parse_ml_comment(TokenCollection &tokens, LexerCursor &cursor);

    /**
     * Parses the given input string into a collection of tokens
     */
    void tokenize(TokenCollection &tokens, const std::string &input);

private:
};

#endif // LEXER_H