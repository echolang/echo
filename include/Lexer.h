#include <iostream>
#include <vector>
#include <string>
#include <regex>
#include <concepts>

#define MHP_VOCAB_LB '\n'
#define MHP_VOCAB_SPACE ' '
#define MHP_VOCAB_TAB '\t'
#define MHP_VOCAB_DUBQUOTE '"'
#define MHP_VOCAB_SNGQUOTE '\''

struct Token {
public:
    enum class Type {
        t_keyword,
        t_identifier,
        t_operator,
        t_punctuation,
        t_whitespace,
        t_comment,
        t_string_literal,
        t_integer_literal,
        t_hex_literal,
        t_binary_literal,
        t_floating_literal,
        t_unknown
    };

    Type type;
    size_t line;
    size_t char_offset;

    Token(Type type, size_t line, size_t char_offset)
        : type(type), line(line), char_offset(char_offset) {}
};

struct LexerRule {
    std::regex pattern;
    Token::Type type;
};

struct TokenCollection {
    std::vector<Token> tokens;
    std::vector<std::string> token_values;

    void push(const std::string &value, Token::Type type, size_t line, size_t char_offset) {
        tokens.emplace_back(type, line, char_offset);
        token_values.push_back(value);
    }
};

struct LexerCursor
{
    size_t line;
    size_t char_offset;
    std::string::const_iterator it;
    const std::string &input;

    LexerCursor(const std::string &input)
        : line(1), char_offset(1), it(input.begin()), input(input) {}

    inline bool is_eof() const {
        return it == input.end();
    }

    inline char peek() const {
        return *it;
    }

    inline char peek(size_t offset) const {
        return *(it + offset);
    }

    inline void skip(size_t offset = 1) {
        if (peek() == MHP_VOCAB_LB) {
            line++;
            char_offset = 1;
        } else {
            char_offset++;
        }

        it += offset;
    }

    inline void skip_formatting() {
        while (is_formatting() && !is_eof()) {
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

    inline bool is_quote() {
        return peek() == MHP_VOCAB_DUBQUOTE || peek() == MHP_VOCAB_SNGQUOTE;
    }

    inline bool is_formatting() {
        return peek() == MHP_VOCAB_SPACE || peek() == MHP_VOCAB_TAB || peek() == MHP_VOCAB_LB;
    }
};


class Lexer 
{
    using LexerFunctionSignature = std::function<bool(Lexer&, TokenCollection&, LexerCursor&)>;

    const std::vector<LexerRule> rules = {
        // { std::regex(R"(^[+\-]?([0-9]*[.])?[0-9]+)"),   Token::Type::t_number },
        { std::regex(R"([a-zA-Z0-9_]\w*)"),             Token::Type::t_identifier },
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

    Lexer() {}

    /**
     * Parses a string literal
     */
    bool parse_string(TokenCollection &tokens, LexerCursor &cursor);

    /**
     * Parses a hex literal
     */
    bool parse_hex(TokenCollection &tokens, LexerCursor &cursor);

    /**
     * Parses the given input string into a collection of tokens
     */
    void tokenize(TokenCollection &tokens, const std::string &input);

private:
};
