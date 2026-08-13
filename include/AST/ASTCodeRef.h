#ifndef ASTCODEREF_H
#define ASTCODEREF_H

#pragma once

#include "AST/ASTModule.h"

#include <optional>
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

    // **the same place in the source, at a different token.** a CodeRef is a module, a file and a token
    // range, so moving a diagnostic to a token the caller already holds is rebasing the range onto the
    // one it was handed - there is nothing else in it to get wrong.
    //
    // it lives beside CodeRef rather than once per pass because the token is routinely optional: a
    // diagnostic that would rather point at the `.` a shorthand was written with still has to be
    // reportable when nothing recorded one, and every caller's fallback is the ref it started from
    inline CodeRef at_token(const CodeRef &at, const std::optional<TokenReference> &token)
    {
        return CodeRef { at.module, at.file, token.has_value() ? token.value().make_slice() : at.token_slice };
    }
};

#endif
