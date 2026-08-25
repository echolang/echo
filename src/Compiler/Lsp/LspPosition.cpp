#include "Compiler/Lsp/LspPosition.h"

#include "AST/ASTFile.h"

namespace
{
    uint32_t utf8_sequence_length(unsigned char lead)
    {
        if ((lead & 0x80) == 0) {
            return 1;
        }
        if ((lead & 0xe0) == 0xc0) {
            return 2;
        }
        if ((lead & 0xf0) == 0xe0) {
            return 3;
        }
        if ((lead & 0xf8) == 0xf0) {
            return 4;
        }

        return 1;
    }

    std::string line_of(const AST::File &file, uint32_t echo_line)
    {
        return file.get_content_of_line(echo_line);
    }
};

uint32_t Compiler::Lsp::utf16_column_of(std::string_view line, uint32_t byte_column)
{
    uint32_t utf16 = 0;
    uint32_t byte = 0;

    while (byte < byte_column && byte < line.size()) {
        const uint32_t width = utf8_sequence_length(static_cast<unsigned char>(line[byte]));
        utf16 += (width == 4) ? 2 : 1;
        byte += width;
    }

    return utf16;
}

uint32_t Compiler::Lsp::byte_column_of_utf16(std::string_view line, uint32_t utf16_column)
{
    uint32_t utf16 = 0;
    uint32_t byte = 0;

    while (utf16 < utf16_column && byte < line.size()) {
        const uint32_t width = utf8_sequence_length(static_cast<unsigned char>(line[byte]));
        utf16 += (width == 4) ? 2 : 1;
        byte += width;
    }

    return byte;
}

Compiler::Lsp::Position Compiler::Lsp::lsp_position_of(
    const AST::File &file,
    AST::Location location,
    bool utf8_encoding
)
{
    Position position;
    position.line = location.line == 0 ? 0 : location.line - 1;

    const uint32_t byte_column = location.column == 0 ? 0 : location.column - 1;

    if (utf8_encoding) {
        position.character = byte_column;
        return position;
    }

    position.character = utf16_column_of(line_of(file, location.line), byte_column);
    return position;
}

uint32_t Compiler::Lsp::byte_column_of(const AST::File &file, Position position, bool utf8_encoding)
{
    if (utf8_encoding) {
        return position.character;
    }

    const uint32_t echo_line = position.line + 1;
    return byte_column_of_utf16(line_of(file, echo_line), position.character);
}

Compiler::Lsp::Range Compiler::Lsp::span_to_lsp_range(const AST::Span &span, bool utf8_encoding)
{
    Range range;

    if (span.file == nullptr) {
        return range;
    }

    range.start = lsp_position_of(*span.file, span.start, utf8_encoding);
    range.end = lsp_position_of(*span.file, span.end, utf8_encoding);
    return range;
}

AST::Location Compiler::Lsp::echo_location_of(
    const AST::File &file,
    Position position,
    bool utf8_encoding
)
{
    AST::Location location;
    location.line = position.line + 1;
    location.column = byte_column_of(file, position, utf8_encoding) + 1;
    return location;
}
