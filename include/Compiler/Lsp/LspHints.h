#ifndef LSPHINTS_H
#define LSPHINTS_H

#pragma once

#include "AST/ASTDiagnostic.h"
#include "AST/ASTFile.h"
#include "Compiler/Lsp/LspSnapshot.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Compiler
{
    namespace Lsp
    {
        // the DocumentHighlightKind numbers
        enum class HighlightKind
        {
            t_text = 1,
            t_read = 2,
            t_write = 3
        };

        struct Highlight
        {
            AST::Span range;
            HighlightKind kind = HighlightKind::t_read;
        };

        struct InlayHint
        {
            // the start of the variable's name: the hint sits in front of it, where `int32 $x`
            // would have written the type
            AST::Location position;
            std::string label;
            std::string tooltip;
        };

        // every mention in this file of what the token at `location` names. snapshot coordinates
        std::vector<Highlight> document_highlights(
            const Snapshot &snapshot,
            const AST::File &file,
            AST::Location location
        );

        // the inferred type of each `$x = ...` declaration on snapshot lines first..last.
        // skipped when the source already wrote the type, including every parameter
        std::vector<InlayHint> inlay_hints(
            const Snapshot &snapshot,
            const AST::File &file,
            uint32_t first_line,
            uint32_t last_line
        );
    };
};

#endif
