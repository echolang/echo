#include "Lexer.h"

constexpr bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') || 
           (c >= 'a' && c <= 'f') || 
           (c >= 'A' && c <= 'F');
}

constexpr std::array<bool, 256> generate_lookup_table() {
    std::array<bool, 256> table = {};
    for (size_t i = 0; i < 256; ++i) {
        table[i] = is_hex_char(static_cast<char>(i));
    }
    return table;
}

constexpr auto hex_lut = generate_lookup_table();


bool Lexer::parse_string(TokenCollection &tokens, LexerCursor &cursor) 
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

bool Lexer::parse_hex(TokenCollection &tokens, LexerCursor &cursor) {

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

void Lexer::tokenize(TokenCollection &tokens, const std::string &input) {
    
    std::vector<Lexer::LexerFunctionSignature> functions = {
        &Lexer::parse_string,
        &Lexer::parse_hex
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
            throw UnknownTokenException { std::string(cursor.it, cursor.it + 10), cursor.line, cursor.char_offset };
        }
    }
}