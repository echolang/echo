#ifndef ASTCODEREF_H
#define ASTCODEREF_H

#pragma once

#include "AST/ASTModule.h"

#include <optional>
#include <tuple>

namespace AST
{
    // where a diagnostic points: a module and the tokens it is about. the file is the first
    // token's, not a third stored field.
    //
    // **it does not render itself.** drawing is AST::DiagnosticRenderer's question, and turning a
    // token slice into a character range is AST::span_of's; what is left here is the reference itself
    struct CodeRef
    {
        const Module *module;
        const TokenSlice token_slice;

        // the file the slice's first token names. a stored copy was a second answer to a question
        // the token now owns, and it could lie: Context::code_ref stamped the *file being parsed*,
        // which is the wrong file when the token is from a sibling of the same module
        const File *file() const {
            return token_slice.start_ref().file();
        }

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

    // **the same place in the source, at a different token.** a CodeRef is a module and a token
    // range, so moving a diagnostic to a token the caller already holds is rebasing the range onto the
    // one it was handed - there is nothing else in it to get wrong.
    //
    // it lives beside CodeRef rather than once per pass because the token is routinely optional: a
    // diagnostic that would rather point at the `.` a shorthand was written with still has to be
    // reportable when nothing recorded one, and every caller's fallback is the ref it started from
    inline CodeRef at_token(const CodeRef &at, const std::optional<TokenReference> &token)
    {
        return CodeRef { at.module, token.has_value() ? token.value().make_slice() : at.token_slice };
    }
};

#endif
