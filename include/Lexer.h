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
        t_equals,                   // =
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

    void clear() {
        tokens.clear();
        token_values.clear();
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
            char_offset += offset;
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
        constexpr auto size = sizeof(lit.value);
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

        std::regex regex(contents);
        std::smatch match;
        if (!std::regex_search(cursor.it, cursor.input.end(), match, regex)) {
            return false;
        }

        tokens.push(match.str(), T, cursor.line, cursor.char_offset);
        cursor.skip(match.str().size());
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
     * Parses the given input string into a collection of tokens
     */
    void tokenize(TokenCollection &tokens, const std::string &input);

private:
};
