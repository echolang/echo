#include "Lexer.h"

constexpr bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') || 
           (c >= 'a' && c <= 'f') || 
           (c >= 'A' && c <= 'F');
}

constexpr bool is_numeric_char(char c) {
    return (c >= '0' && c <= '9');
}

constexpr bool is_valid_varname_char(char c) {
    return (c >= 'a' && c <= 'z') || 
           (c >= 'A' && c <= 'Z') || 
           (c >= '0' && c <= '9') || 
           (c == '_');
}

constexpr std::array<bool, 256> generate_hex_lut() {
    std::array<bool, 256> table = {};
    for (size_t i = 0; i < 256; ++i) {
        table[i] = is_hex_char(static_cast<char>(i));
    }
    return table;
}

constexpr std::array<bool, 256> generate_numeric_lut() {
    std::array<bool, 256> table = {};
    for (size_t i = 0; i < 256; ++i) {
        table[i] = is_numeric_char(static_cast<char>(i));
    }
    return table;
}

constexpr std::array<bool, 256> generate_varname_lut() {
    std::array<bool, 256> table = {};
    for (size_t i = 0; i < 256; ++i) {
        table[i] = is_valid_varname_char(static_cast<char>(i));
    }
    return table;
}

constexpr auto hex_lut = generate_hex_lut();
constexpr auto numeric_lut = generate_numeric_lut();
constexpr auto varname_lut = generate_varname_lut();

bool Lexer::parse_varname(TokenCollection &tokens, LexerCursor &cursor)
{
    // just use a regex for now
    if (cursor.peek() != '$') {
        return false;
    }

    auto start = cursor.it;
    cursor.skip();

    while (!cursor.is_eof() && varname_lut[cursor.peek()]) {
        cursor.skip();
    }

    tokens.push(std::string(start, cursor.it), Token::Type::t_varname, cursor.line, cursor.char_offset);

    return true;
}

bool Lexer::parse_string_literal(TokenCollection &tokens, LexerCursor &cursor) 
{
    if (!cursor.is_quote()) {
        return false;
    }

    const auto string_start_offset = cursor.char_offset;
    const auto string_start_line = cursor.line;

    auto start = cursor.it;
    auto quote = cursor.peek();
    cursor.skip();

    while (true) {
        if (cursor.is_eof()) {
            throw UnterminatedStringException { std::string(start, cursor.it), string_start_line, string_start_offset };
        }

        if (cursor.peek() == quote) {
            cursor.skip();
            break;
        }

        if (cursor.peek() == '\\') {
            cursor.skip();
        }

        cursor.skip();
    }

    tokens.push(std::string(start, cursor.it), Token::Type::t_string_literal, string_start_line, string_start_offset);

    return true;
}

bool Lexer::parse_hex_literal(TokenCollection &tokens, LexerCursor &cursor) {

    if (!cursor.begins_with("0x")) {
        return false;
    }

    cursor.skip(2);
    std::string value = "0x";

    while (hex_lut[cursor.peek()]) {
        value += cursor.peek();
        cursor.skip();
    }

    tokens.push(value, Token::Type::t_hex_literal, cursor.line, cursor.char_offset);

    return true;
}

bool Lexer::parse_sl_comment(TokenCollection &tokens, LexerCursor &cursor)
{
    if (!cursor.begins_with("//")) {
        return false;
    }

    cursor.skip(2);
    cursor.skip_until('\n');

    return true;
}

bool Lexer::parse_ml_comment(TokenCollection &tokens, LexerCursor &cursor)
{
    return false;
}

void Lexer::tokenize(TokenCollection &tokens, const std::string &input) {
    
    std::vector<Lexer::LexerFunctionSignature> functions = {
        &Lexer::parse_string_literal,
        &Lexer::parse_varname,
        &Lexer::parse_sl_comment,
        &Lexer::parse_ml_comment,
        &Lexer::parse_char_token<';', Token::Type::t_semicolon>,
        &Lexer::parse_char_token<':', Token::Type::t_colon>,
        &Lexer::parse_char_token<',', Token::Type::t_comma>,
        &Lexer::parse_char_token<'.', Token::Type::t_dot>,
        &Lexer::parse_exact_token<"&&", Token::Type::t_logical_and>,
        &Lexer::parse_exact_token<"||", Token::Type::t_logical_or>,
        &Lexer::parse_exact_token<"==", Token::Type::t_logical_eq>,
        &Lexer::parse_exact_token<"!=", Token::Type::t_logical_neq>,
        &Lexer::parse_exact_token<"<=", Token::Type::t_logical_leq>,
        &Lexer::parse_exact_token<">=", Token::Type::t_logical_geq>,
        &Lexer::parse_char_token<'=', Token::Type::t_assign>,
        &Lexer::parse_exact_token<"++", Token::Type::t_op_inc>,
        &Lexer::parse_exact_token<"--", Token::Type::t_op_dec>,
        &Lexer::parse_char_token<'+', Token::Type::t_op_add>,
        &Lexer::parse_char_token<'-', Token::Type::t_op_sub>,
        &Lexer::parse_char_token<'*', Token::Type::t_op_mul>,
        &Lexer::parse_char_token<'/', Token::Type::t_op_div>,
        &Lexer::parse_char_token<'%', Token::Type::t_op_mod>,
        &Lexer::parse_char_token<'?', Token::Type::t_qmark>,
        &Lexer::parse_char_token<'!', Token::Type::t_exclamation>,
        &Lexer::parse_char_token<'<', Token::Type::t_open_angle>,
        &Lexer::parse_char_token<'>', Token::Type::t_close_angle>,
        &Lexer::parse_char_token<'(', Token::Type::t_open_paren>,
        &Lexer::parse_char_token<')', Token::Type::t_close_paren>,
        &Lexer::parse_char_token<'{', Token::Type::t_open_brace>,
        &Lexer::parse_char_token<'}', Token::Type::t_close_brace>,
        &Lexer::parse_char_token<'[', Token::Type::t_open_bracket>,
        &Lexer::parse_char_token<']', Token::Type::t_close_bracket>,
        &Lexer::parse_exact_token<"true", Token::Type::t_bool_literal>,
        &Lexer::parse_exact_token<"false", Token::Type::t_bool_literal>,
        &Lexer::parse_hex_literal,
        &Lexer::parse_regex_token<"^[0-9]+\\.[0-9]+f?", Token::Type::t_floating_literal>,
        &Lexer::parse_regex_token<"^[0-9]+", Token::Type::t_integer_literal>,
        // generic identifier
        &Lexer::parse_regex_token<"^[_a-zA-Z0-9]+", Token::Type::t_identifier>
    };
    
    auto cursor = LexerCursor(input);

    while (!cursor.is_eof()) 
    {
        // formatting aka whitespace, tabs, newlines
        if (cursor.is_formatting()) {
            cursor.skip_formatting();
        }

        bool matched = false;

        for (auto &func : functions) {
            if (func(*this, tokens, cursor)) {
                matched = true;
                break;
            }
        }

        if (!matched) {
            // construct a string with some context around the unknown token
            if (cursor.it + 10 < cursor.input.end()) {
                throw UnknownTokenException { std::string(cursor.it, cursor.it + 10), cursor.line, cursor.char_offset };
            } else {
                throw UnknownTokenException { std::string(cursor.it, cursor.input.end()), cursor.line, cursor.char_offset };
            }
        }
    }
}