#ifndef LEXER_H
#define LEXER_H

#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <exception>

#include "Token.h"
#include "AST/ASTOps.h"

#define MHP_VOCAB_LB '\n'
#define MHP_VOCAB_SPACE ' '
#define MHP_VOCAB_TAB '\t'
#define MHP_VOCAB_DUBQUOTE '"'
#define MHP_VOCAB_SNGQUOTE '\''

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

    // returns a string giving the user some context
    // of where in the code the iterator currently is, will never go beyond its current line
    std::string get_code_sample(const std::string::const_iterator it, const uint32_t start_offset = 0, const uint32_t end_offset = 20) const;

private:
    std::string::const_iterator it;
};

class LexerEngine;

namespace LexerFunction
{
    class Base
    {
    public:
        virtual ~Base() {};

        // the priority of the lexer function, wil be used to sort
        // all considered lexer functions
        virtual int priority() const = 0;

        // defines what prefix needs to match entirely for this lexer function
        // to be called, if an empty string is returned the function will be considered
        // everywhere (for example for identifiers)
        virtual const std::vector<std::string> must_match() const = 0;

        // lex tha stuff
        virtual bool parse(TokenCollection &tokens, LexerCursor &cursor) const = 0;

        // handed the engine that owns this function, once its trie is built. a no-op for every
        // function that lexes a flat run of characters, which is all of them but one: a hole
        // inside an interpolated string literal is ordinary Echo and needs the whole vocabulary
        // back. handed over rather than constructed with, since the function has to be in the
        // list the trie is built from
        virtual void bind_engine(const LexerEngine &engine) { (void)engine; }
    };

    struct TreeNode
    {
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

    // a StringToken that only matches when the literal is a complete word. every keyword spelled
    // out of identifier characters is one of these; the symbol keywords (`::`, `:$`, `#`, ...)
    // stay plain StringTokens, since no identifier can start with them
    class KeywordToken : public StringToken
    {
    public:
        KeywordToken(const std::string lit, const Token::Type type) : StringToken(lit, type) {}

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

    // the same shape as HexLiteral, and it has to sit at the same priority for the same reason:
    // NumericLiteral matches at every position, so without this `0b1011` lexed as the integer `0`
    // followed by an identifier `b1011` - two tokens, no diagnostic at the lexer, and a parser stub
    // that fell off the end of a non-void function if the token had ever been produced
    class BinaryLiteral : public Base
    {
    public:
        int priority() const override {
            return 20;
        }

        const std::vector<std::string> must_match() const override;
        bool parse(TokenCollection &tokens, LexerCursor &cursor) const override;
    };

    // **the two quote characters are not interchangeable.** a `'` string is verbatim and lexes to
    // exactly one t_string_literal; a `"` string interpolates `{$...}` and lexes to a *run* of
    // tokens when it holds one. a `"` string with no `{$` in it takes the verbatim path too, which
    // is what keeps every program written before interpolation existed byte for byte the same
    class StringLiteral : public Base
    {
    public:
        int priority() const override {
            return 5;
        }

        const std::vector<std::string> must_match() const override;
        bool parse(TokenCollection &tokens, LexerCursor &cursor) const override;

        void bind_engine(const LexerEngine &engine) override {
            _engine = &engine;
        }

    private:
        // does this literal hold a `{$` before the quote that would close it? answered by a
        // look-ahead rather than by lexing, because the answer decides which of the two shapes the
        // whole literal takes and there is no way back once a token has been pushed
        bool holds_a_hole(const LexerCursor &cursor, char quote) const;

        // the verbatim shape: quotes kept, escapes untouched, one token
        void lex_verbatim(TokenCollection &tokens, LexerCursor &cursor, char quote) const;

        // one hole, from just after its `{$` through the `}` that closes it. pushes the hole's own
        // tokens, and a t_string_interp_spec when a top level `:` introduced a format spec
        void lex_hole(TokenCollection &tokens, LexerCursor &cursor) const;

        const LexerEngine *_engine = nullptr;
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
};

// the lexer function list and the character trie built out of it, as one object that can be asked
// for **one token** rather than only for a whole file.
//
// it exists because lexing is no longer flat: a hole inside an interpolated string literal holds
// ordinary Echo, and the only thing that can lex ordinary Echo is the vocabulary the top level uses.
// so `LexerFunction::StringLiteral` is handed this engine and steps it, which keeps the hole's
// tokens carrying **real source positions** - the thing a re-lex of the token's text afterwards
// could never do, and what every diagnostic and the `-g` line table read
class LexerEngine
{
public:
    typedef std::vector<std::unique_ptr<LexerFunction::Base>> FunctionList;

    static constexpr size_t MAX_PREFIX_LENGTH = 3;

    // builds the trie over `functions` and binds itself into every one of them. the list must
    // outlive the engine - it holds the functions, this only points at them
    LexerEngine(FunctionList &functions);

    // to end of input
    void run(TokenCollection &tokens, LexerCursor &cursor) const;

    // one token, chosen the way `run` chooses it. false when no function matched, which is the
    // caller's to report - `run` says "unknown token", a hole says where the hole was
    bool step(TokenCollection &tokens, LexerCursor &cursor) const;

private:
    std::unique_ptr<LexerFunction::TreeNode> _root;
};

class Lexer
{
public:
    struct TokenException : public std::exception
    {
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

        size_t get_line() const {
            return line;
        }

        size_t get_char_offset() const {
            return char_offset;
        }

    private:
        std::string snippet;
        size_t line;
        size_t char_offset;
        std::string error_message;
    };

    struct UnknownTokenException : public TokenException
    {
        UnknownTokenException(const std::string &snippet, size_t line, size_t char_offset) :
            TokenException("Unknown token at line " + std::to_string(line) + " offset " + std::to_string(char_offset) + " near: " + snippet, snippet, line, char_offset)
        {}
    };

    struct UnterminatedStringException : public TokenException
    {
        UnterminatedStringException(const std::string &snippet, size_t line, size_t char_offset) :
            TokenException("Unterminated string at line " + std::to_string(line) + " offset " + std::to_string(char_offset) + " near: " + snippet, snippet, line, char_offset)
        {}
    };

    struct UnterminatedCommentException : public TokenException
    {
        UnterminatedCommentException(const std::string &snippet, size_t line, size_t char_offset) :
            TokenException("Unterminated comment at line " + std::to_string(line) + " offset " + std::to_string(char_offset) + " near: " + snippet, snippet, line, char_offset)
        {}
    };

    struct UnterminatedInterpolationException : public TokenException
    {
        UnterminatedInterpolationException(const std::string &snippet, size_t line, size_t char_offset) :
            TokenException("Unterminated interpolation at line " + std::to_string(line) + " offset " + std::to_string(char_offset) + " near: " + snippet, snippet, line, char_offset)
        {}
    };

    Lexer() {}
    ~Lexer() {}

    /**
     * Parses the given input string into a collection of tokens
     */
    void tokenize(TokenCollection &tokens, const std::string &input);
};

#endif // LEXER_H
