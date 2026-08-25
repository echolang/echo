#ifndef LSPPOSITION_H
#define LSPPOSITION_H

#pragma once

#include "AST/ASTDiagnostic.h"

#include <cstdint>
#include <string_view>

namespace Compiler
{
    namespace Lsp
    {
        // 0-based, in the encoding the session negotiated. UTF-16 is always implemented;
        // utf-8 is used when the client offered it
        struct Position
        {
            uint32_t line = 0;
            uint32_t character = 0;
        };

        struct Range
        {
            Position start;
            Position end;
        };

        // byte ↔ UTF-16 over one line of source. a 4-byte UTF-8 scalar is two UTF-16
        // units; everything in the BMP is one. string-literal spans are two bytes short
        // (the quotes) - known, accepted, the same fact AST::span_of documents
        uint32_t utf16_column_of(std::string_view line, uint32_t byte_column);
        uint32_t byte_column_of_utf16(std::string_view line, uint32_t utf16_column);

        Position lsp_position_of(const AST::File &file, AST::Location location, bool utf8_encoding);
        uint32_t byte_column_of(const AST::File &file, Position position, bool utf8_encoding);
        Range span_to_lsp_range(const AST::Span &span, bool utf8_encoding);
        AST::Location echo_location_of(const AST::File &file, Position position, bool utf8_encoding);
    };
};

#endif
