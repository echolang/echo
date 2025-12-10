#ifndef ASTCODEREF_H
#define ASTCODEREF_H

#pragma once

#include "ASTModule.h"

#include <tuple>

namespace AST
{
    // where a diagnostic points: a module, the file it was written in, and the tokens it is about.
    //
    // **it does not render itself.** It used to - `get_referenced_code_excerpt()` drew the source frame
    // here, inline in this header, with a caret indented by a hardcoded five spaces against a gutter whose
    // width grew with the line number, so every two-digit line was off by one. Drawing is
    // AST::DiagnosticRenderer's question now, and turning a token slice into a character range is
    // AST::span_of's; what is left here is the reference itself
    struct CodeRef
    {
        const Module *module;
        const File *file;
        const TokenSlice token_slice;

        std::tuple<uint32_t, uint32_t> line_range() const
        {
            return std::make_tuple(token_slice.startt().line, token_slice.endt().line);
        }

        std::tuple<uint32_t, uint32_t> char_offset_range() const
        {
            return std::make_tuple(token_slice.startt().char_offset, token_slice.endt().char_offset);
        }

        bool is_single_line() const
        {
            return token_slice.startt().line == token_slice.endt().line;
        }

        bool is_single_token() const
        {
            return token_slice.startt().char_offset == token_slice.endt().char_offset;
        }
    };
};

#endif
