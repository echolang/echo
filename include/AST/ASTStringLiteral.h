#ifndef ASTSTRINGLITERAL_H
#define ASTSTRINGLITERAL_H

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace AST
{
    // what went wrong decoding a string literal. `offset` is a byte offset *into the quoted token*, so a
    // diagnostic can point at the offending escape rather than at the whole literal
    struct StringLiteralError
    {
        std::string message;
        size_t offset;
    };

    // the bytes a string literal denotes: the surrounding quotes stripped and every escape decoded.
    //
    // the one owner of that question, because the byte count it produces is load-bearing twice over -
    // `string`'s `$size` is the *decoded* length, and UTF-8 validity is a property of the decoded bytes
    // rather than of the source text. the lexer deliberately keeps the token verbatim (a code excerpt
    // has to show what was written), so this is where source becomes value.
    //
    // returns nullopt on success, having filled `out_bytes`. on failure `out_bytes` holds whatever was
    // decoded up to the error, which keeps a caller that reports and continues from reading garbage
    //
    // recognised: \n \t \r \\ \" \' \0 \xNN \u{...}. an unrecognised escape is an error rather than
    // silently the character itself - that is the difference between a typo you are told about and one
    // that ships
    std::optional<StringLiteralError> decode_string_literal(const std::string &quoted, std::string &out_bytes);

    // is this a well-formed UTF-8 sequence? answers the byte offset of the first problem, or nullopt.
    // rejects overlong encodings, surrogate halves and anything above U+10FFFF, not merely bad
    // continuation bytes - a `string` promises its bytes are valid UTF-8, and every `char_at` walk
    // relies on it rather than re-checking
    std::optional<size_t> utf8_first_invalid(const std::string &bytes);

    // appends `codepoint` to `out` as UTF-8. false when it is not a legal scalar value
    bool utf8_encode(uint32_t codepoint, std::string &out);
};

#endif
