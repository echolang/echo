#ifndef LEXER_H
#define LEXER_H

#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <cstring>

#include "Token.h"
#include "AST/ASTOps.h"

#define MHP_VOCAB_LB '\n'
#define MHP_VOCAB_SPACE ' '
#define MHP_VOCAB_TAB '\t'
#define MHP_VOCAB_DUBQUOTE '"'
#define MHP_VOCAB_SNGQUOTE '\''

// struct LexerRule {
//     std::regex pattern;
//     Token::Type type;
// };

struct LexerCursor
{
    size_t line;
    size_t char_offset;
    size_t end_of_line_offset;

    const std::string &input;

    LexerCursor(const std::string &input) : 
        line(1), char_offset(1), input(input), it(input.begin())
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
        return (it != input.end()) ? *it : '\0';
    }

    inline char peek(size_t offset) const {
        return (it + offset != input.end()) ? *(it + offset) : '\0';
    }

    inline const std::string::const_iterator current() const {
        return it;
    }

    void reset();

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
    
        if (it + offset > input.end()) {
            it = input.end();
        } else {
            it += offset;
        }

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

    inline void skip_until_nl() {
        skip_until(MHP_VOCAB_LB);
    }

    // will skip the currently begining scope and move the cursor to the end of the scope
    // if the cursor is not at the beginning of a scope, the cursor will not be moved at all
    void skip_scope(const char open, const char close);

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

    // to be able to handle expressions properly we need to have some rule set of what is a valid
    // ending of a token. This should allow us to identify custom operators even if they are built from 
    // prexisting operators (e.g. "<=") for example "<=>" should be identified as a single token and not as "<=" and ">"
    // Now one way would be to run the lexer twice, once to identify the custom operators and then to tokenize the input
    // with those additional operators in mind. But I much more prefer to have the lexer identify these tokens in a single pass
    //
    // This might be a bit of a hack and it kinda breaks one rule is set myself in the beginning being "formatting characters should not be part of the syntax"
    // but here me out:
    //   $foo = 1 <=++2; 
    //   $foo = 1 <= ++2; 
    //              | The space makes it clear that these are two separate tokens
    //   $foo = (1++ + 2++); // (++, +, ++) not (+++, ++) 
    // i just hope that I won't come back to this and think "what a fucking idiot"
    // 
    // Update a weeek later:
    // Turns out im an idiot, running the lexer twice with another set of lexing functions 
    // is far easier and lets me identifiy custom operators early on in the full lexing pass. 
    // keeping this here for the record and to remind myself that our mindset should be "we are dump till proven wrong" :)
    bool is_seperating_char(size_t offset = 0);

    // returns a string giving the user some context 
    // of where in the code the iterator currently is, will never go beyond its current line
    std::string get_code_sample(const std::string::const_iterator it, const uint32_t start_offset = 0, const uint32_t end_offset = 20) const;

private:
    std::string::const_iterator it;
};

namespace LexerFunction
{
    class Base 
    {
    public:
        virtual ~Base() {};

        // the priority of the lexer function, wil be used to sort
        // all considered lexer functions.
        virtual int priority() const = 0;

        // defines what prefix needs to match entirely for this lexer function
        // to be called, if an empty string is returned the function will be considered
        // everywhere (for example for identifiers)
        virtual const std::vector<std::string> must_match() const = 0;

        // lex tha stuff
        virtual bool parse(TokenCollection &tokens, LexerCursor &cursor) const = 0;
    };

    struct TreeNode {
        std::string prefix;
        std::vector<Base *> functions;
        std::unordered_map<char, std::unique_ptr<TreeNode>> children;

        TreeNode(const std::string &prefix) : prefix(prefix) {}
        TreeNode() = default;
    };

    class CharToken : public Base
    {
    public:
        const char lit;
        const Token::Type type;

        CharToken(const char lit, const Token::Type type) : lit(lit), type(type) {
            // must be a non null character
            assert(lit != '\0');
        }

        int priority() const override {
            return 0; // default priority
        }

        const std::vector<std::string> must_match() const override;
        bool parse(TokenCollection &tokens, LexerCursor &cursor) const override;
    };

    class StringToken : public Base
    {
    public:
        const std::string lit;
        const Token::Type type;

        StringToken(const std::string lit, const Token::Type type) : lit(lit), type(type) {}

        int priority() const override {
            return lit.size(); // the longer the string the higher the priority
        }

        const std::vector<std::string> must_match() const override;
        bool parse(TokenCollection &tokens, LexerCursor &cursor) const override;
    };

    class NumericLiteral : public Base
    {
    public:
        int priority() const override {
            return 10; // a negative number literal should be considered before the neg operator
        }

        const std::vector<std::string> must_match() const override;
        bool parse(TokenCollection &tokens, LexerCursor &cursor) const override;
    };

    class HexLiteral : public Base
    {
    public:
        int priority() const override {
            return 20;
        }

        const std::vector<std::string> must_match() const override;
        bool parse(TokenCollection &tokens, LexerCursor &cursor) const override;
    };

    class StringLiteral : public Base
    {
    public:
        int priority() const override {
            return 5;
        }

        const std::vector<std::string> must_match() const override;
        bool parse(TokenCollection &tokens, LexerCursor &cursor) const override;
    };

    class VariableName : public Base
    {
    public:
        int priority() const override {
            return 100;
        }

        const std::vector<std::string> must_match() const override;
        bool parse(TokenCollection &tokens, LexerCursor &cursor) const override;
    };

    class ReferenceFrom : public Base
    {
    public:
        int priority() const override {
            return 50;
        }

        const std::vector<std::string> must_match() const override;
        bool parse(TokenCollection &tokens, LexerCursor &cursor) const override;
    };

    class SingleLineComment : public Base
    {
    public:
        int priority() const override {
            return 999;
        }

        const std::vector<std::string> must_match() const override;
        bool parse(TokenCollection &tokens, LexerCursor &cursor) const override;
    };

    class MultiLineComment : public Base
    {
    public:
        int priority() const override {
            return 999;
        }

        const std::vector<std::string> must_match() const override;
        bool parse(TokenCollection &tokens, LexerCursor &cursor) const override;
    };

    class Identifier : public Base
    {
    public:
        int priority() const override {
            return -1;
        }

        const std::vector<std::string> must_match() const override;
        bool parse(TokenCollection &tokens, LexerCursor &cursor) const override;
    };
}

class Lexer 
{
    using FunctionList = std::vector<std::unique_ptr<LexerFunction::Base>>;

    // template<size_t N>
    // struct CSXStrLiteral {
    //     constexpr CSXStrLiteral(const char (&str)[N]) {
    //         std::copy_n(str, N, value);
    //     }
    //     char value[N];
    // };
    // using LexerFunctionSignature = std::function<bool(Lexer&, TokenCollection&, LexerCursor&)>;
    
    const uint32_t MAX_PREFIX_LENGTH = 3;

public:
    struct TokenException : public std::exception {
        TokenException(const std::string &message, const std::string &snippet, size_t line, size_t char_offset) :
            snippet(snippet), 
            line(line), 
            char_offset(char_offset),
            error_message(message)
        {}

        const char *what() const noexcept override {
            return error_message.c_str();
        }

        const std::string &get_snippet() const {
            return snippet;
        }

    private:
        std::string snippet;
        size_t line;
        size_t char_offset;
        std::string error_message;
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

    struct UnterminatedCommentException : public TokenException {
        UnterminatedCommentException(const std::string &snippet, size_t line, size_t char_offset) :
            TokenException("Unterminated comment at line " + std::to_string(line) + " offset " + std::to_string(char_offset) + " near: " + snippet, snippet, line, char_offset)
        {}
    };

    Lexer() {}
    ~Lexer() {}

    /**
     * Executes the given function list
     */
    void execute_functions(FunctionList &functions, TokenCollection &tokens, LexerCursor &cursor);


    // /**
    //  * Returns a single character parser function, useful for (<, >, ?, etc.)
    //  */
    // template <char C, Token::Type T>
    // bool parse_char_token(TokenCollection &tokens, LexerCursor &cursor) {
    //     if (cursor.peek() != C) {
    //         return false;
    //     }

    //     tokens.push(std::string(1, C), T, cursor.line, cursor.char_offset);
    //     cursor.skip();
    //     return true;
    // }

    // /**
    //  * Same as "parse_char_token" but only accepts the token if it ends with a seperating character
    //  */
    // template <char C, Token::Type T>
    // bool parse_char_token_seperating(TokenCollection &tokens, LexerCursor &cursor) {
    //     if (cursor.peek() != C) {
    //         return false;
    //     }

    //     if (!cursor.is_seperating_char(1)) {
    //         return false;
    //     }

    //     tokens.push(std::string(1, C), T, cursor.line, cursor.char_offset);
    //     cursor.skip();
    //     return true;
    // }

    // /**
    //  * Parses a multi-character token (e.g. "==" or "!=")
    //  */
    // template <CSXStrLiteral lit, Token::Type T>
    // bool parse_exact_token(TokenCollection &tokens, LexerCursor &cursor) {
    //     constexpr auto size = sizeof(lit.value) - 1;
    //     constexpr auto contents = lit.value;

    //     if (!cursor.begins_with(contents)) {
    //         return false;
    //     }

    //     tokens.push(std::string(contents, size), T, cursor.line, cursor.char_offset);
    //     cursor.skip(size);
    //     return true;
    // }

    // /**
    //  * Same as "parse_exact_token" but only accepts the token if it ends with a seperating character
    //  */
    // template <CSXStrLiteral lit, Token::Type T>
    // bool parse_exact_token_seperating(TokenCollection &tokens, LexerCursor &cursor) {
    //     constexpr auto size = sizeof(lit.value) - 1;
    //     constexpr auto contents = lit.value;

    //     if (!cursor.begins_with(contents)) {
    //         return false;
    //     }

    //     if (!cursor.is_seperating_char(size)) {
    //         return false;
    //     }

    //     tokens.push(std::string(contents, size), T, cursor.line, cursor.char_offset);
    //     cursor.skip(size);
    //     return true;
    // }

    // /**
    //  * Parses a token with the given regex pattern
    //  */
    // template <CSXStrLiteral pattern, Token::Type T>
    // bool parse_regex_token(TokenCollection &tokens, LexerCursor &cursor) {
    //     // constexpr auto size = sizeof(pattern.value);
    //     constexpr auto contents = pattern.value;

    //     static std::regex regex(contents, std::regex_constants::optimize);

    //     std::smatch match;
    //     if (!std::regex_search(cursor.it, cursor.end_of_line(), match, regex)) {
    //         return false;
    //     }

    //     auto match_str = match.str();

    //     tokens.push(match_str, T, cursor.line, cursor.char_offset);
    //     cursor.skip(match_str.size());
    //     return true;
    // }

    // /**
    //  * Parse variable names
    //  */
    // bool parse_varname(TokenCollection &tokens, LexerCursor &cursor);

    // /**
    //  * Parses a string literal
    //  */
    // bool parse_string_literal(TokenCollection &tokens, LexerCursor &cursor);

    // /**
    //  * Parses a hex literal
    //  */
    // bool parse_hex_literal(TokenCollection &tokens, LexerCursor &cursor);

    // /**
    //  * Parses a single line comment
    //  */
    // bool parse_sl_comment(TokenCollection &tokens, LexerCursor &cursor);

    // /**
    //  * Parses a multi-line comment
    //  */
    // bool parse_ml_comment(TokenCollection &tokens, LexerCursor &cursor);

    /**
     * Parses the given input string into a collection of tokens
     */
    void tokenize(TokenCollection &tokens, const std::string &input, const AST::OperatorRegistry *op_registry = nullptr);

    /**
     * Tokenizer prepass (used to identify custom operators)
     */
    void tokenize_prepass_operators(const std::string &input, AST::OperatorRegistry &op_registry);

private:
};

#endif // LEXER_H