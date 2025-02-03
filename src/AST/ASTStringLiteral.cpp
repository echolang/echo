#include "AST/ASTStringLiteral.h"

#include <fmt/core.h>

namespace
{
    // the number of bytes the sequence starting with `lead` occupies, or 0 when `lead` cannot start one.
    // a continuation byte (10xxxxxx) answers 0 too, which is what makes a stray one an error rather than
    // the start of something
    size_t utf8_sequence_length(unsigned char lead)
    {
        if (lead < 0x80) return 1;
        if ((lead & 0xE0) == 0xC0) return 2;
        if ((lead & 0xF0) == 0xE0) return 3;
        if ((lead & 0xF8) == 0xF0) return 4;

        return 0;
    }

    bool is_hex_digit(char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    uint32_t hex_value(char c)
    {
        if (c >= '0' && c <= '9') return static_cast<uint32_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint32_t>(c - 'a' + 10);

        return static_cast<uint32_t>(c - 'A' + 10);
    }
}

bool AST::utf8_encode(uint32_t codepoint, std::string &out)
{
    // a surrogate half is not a scalar value: it only means anything as part of a UTF-16 pair, and
    // encoding one produces bytes no decoder should accept (CESU-8). rejected here so it cannot enter a
    // `string` through `\u{d800}`
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return false;
    }

    if (codepoint < 0x80) {
        out += static_cast<char>(codepoint);
    }
    else if (codepoint < 0x800) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else if (codepoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }

    return true;
}

std::optional<size_t> AST::utf8_first_invalid(const std::string &bytes)
{
    size_t i = 0;

    while (i < bytes.size()) {
        const unsigned char lead = static_cast<unsigned char>(bytes[i]);
        const size_t length = utf8_sequence_length(lead);

        if (length == 0 || i + length > bytes.size()) {
            return i;
        }

        // decode as we validate: the overlong and range checks below are about the *value*, and
        // rebuilding it here is cheaper than a second pass keyed on the lead byte's shape
        uint32_t codepoint = length == 1 ? lead : (lead & (0xFF >> (length + 1)));

        for (size_t k = 1; k < length; ++k) {
            const unsigned char continuation = static_cast<unsigned char>(bytes[i + k]);

            if ((continuation & 0xC0) != 0x80) {
                return i;
            }

            codepoint = (codepoint << 6) | (continuation & 0x3F);
        }

        // an overlong encoding spells a small value in more bytes than it needs. two encodings of one
        // character would make byte length and equality disagree, so they are invalid, not merely odd
        static const uint32_t minimum_for_length[5] = { 0, 0, 0x80, 0x800, 0x10000 };

        if (codepoint < minimum_for_length[length]
            || codepoint > 0x10FFFF
            || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return i;
        }

        i += length;
    }

    return std::nullopt;
}

std::optional<AST::StringLiteralError> AST::decode_string_literal(const std::string &quoted, std::string &out_bytes)
{
    out_bytes.clear();

    // the lexer guarantees a matching pair of quotes, so anything shorter than two characters is a
    // malformed token rather than an empty literal
    if (quoted.size() < 2) {
        return AST::StringLiteralError { "malformed string literal", 0 };
    }

    out_bytes.reserve(quoted.size() - 2);

    // walk the token's interior; `i` stays a token offset throughout so an error can be pointed at
    size_t i = 1;
    const size_t end = quoted.size() - 1;

    while (i < end) {
        const char c = quoted[i];

        if (c != '\\') {
            out_bytes += c;
            i += 1;
            continue;
        }

        if (i + 1 >= end) {
            return AST::StringLiteralError { "string literal ends with a trailing '\\'", i };
        }

        const char escape = quoted[i + 1];
        i += 2;

        switch (escape) {
            case 'n':  out_bytes += '\n'; continue;
            case 't':  out_bytes += '\t'; continue;
            case 'r':  out_bytes += '\r'; continue;
            case '0':  out_bytes += '\0'; continue;
            case '\\': out_bytes += '\\'; continue;
            case '"':  out_bytes += '"';  continue;
            case '\'': out_bytes += '\''; continue;

            case 'x': {
                // exactly two hex digits, so `"\x41z"` cannot quietly swallow the `z`
                if (i + 1 >= end || !is_hex_digit(quoted[i]) || !is_hex_digit(quoted[i + 1])) {
                    return AST::StringLiteralError {
                        "'\\x' needs exactly two hex digits, as in '\\x41'", i - 2 };
                }

                // a raw byte, deliberately not a codepoint: `\xNN` is how a specific byte is written,
                // and the UTF-8 check at the end is what decides whether the result is a legal string
                out_bytes += static_cast<char>((hex_value(quoted[i]) << 4) | hex_value(quoted[i + 1]));
                i += 2;
                continue;
            }

            case 'u': {
                const size_t escape_start = i - 2;

                if (i >= end || quoted[i] != '{') {
                    return AST::StringLiteralError {
                        "'\\u' needs a braced codepoint, as in '\\u{1F600}'", escape_start };
                }

                i += 1; // the `{`

                uint32_t codepoint = 0;
                size_t digits = 0;

                while (i < end && is_hex_digit(quoted[i])) {
                    // capped at six digits, which is every legal codepoint, so a long run cannot
                    // silently overflow the accumulator on its way to being rejected
                    if (digits == 6) {
                        return AST::StringLiteralError {
                            "'\\u{...}' takes at most six hex digits", escape_start };
                    }

                    codepoint = (codepoint << 4) | hex_value(quoted[i]);
                    digits += 1;
                    i += 1;
                }

                if (digits == 0) {
                    return AST::StringLiteralError {
                        "'\\u{...}' needs at least one hex digit", escape_start };
                }

                if (i >= end || quoted[i] != '}') {
                    return AST::StringLiteralError { "'\\u{...}' is missing its closing '}'", escape_start };
                }

                i += 1; // the `}`

                if (!AST::utf8_encode(codepoint, out_bytes)) {
                    return AST::StringLiteralError {
                        fmt::format("U+{:04X} is not a valid unicode scalar value", codepoint),
                        escape_start };
                }

                continue;
            }

            default:
                return AST::StringLiteralError {
                    fmt::format("unknown escape sequence '\\{}'", escape), i - 2 };
        }
    }

    // last, on the decoded bytes: `\xNN` can build an invalid sequence out of individually fine
    // escapes, and a literal pasted from a broken source can be invalid with no escape in it at all
    if (auto invalid = AST::utf8_first_invalid(out_bytes)) {
        return AST::StringLiteralError {
            fmt::format("string literal is not valid UTF-8 (at byte {})", invalid.value()), 0 };
    }

    return std::nullopt;
}
