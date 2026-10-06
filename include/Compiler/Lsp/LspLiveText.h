#ifndef LSPLIVETEXT_H
#define LSPLIVETEXT_H

#pragma once

#include "AST/ASTDiagnostic.h"
#include "Compiler/Lsp/LspPosition.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AST
{
    class File;
};

namespace Compiler
{
    namespace Lsp
    {
        class Session;

        // the text the editor has now. split into lines the way AST::File does: on '\n' only, a
        // '\r' stays on its line, so a line of one and a line of the other compare equal when they
        // are the same bytes
        class LiveText
        {
        public:

            explicit LiveText(std::string text);

            const std::string &text() const {
                return _text;
            }

            size_t line_count() const {
                return _line_offsets.size();
            }

            // 1-based, "" for line 0 and past the end, as AST::File::get_content_of_line
            std::string_view line(uint32_t echo_line) const;

            // 1-based line and byte column to a byte offset into text(), and back. clamped to the text
            size_t offset_of(AST::Location location) const;
            AST::Location location_of(size_t offset) const;

        private:

            std::string _text;
            std::vector<size_t> _line_offsets;
        };

        // LspPosition's conversions, against the live line. a UTF-16 column on a line the user just
        // typed an umlaut into is only right measured on that line
        AST::Location live_location_of(const LiveText &live, Position position, bool utf8_encoding);
        Position live_position_of(const LiveText &live, AST::Location location, bool utf8_encoding);

        // which live line is which snapshot line. the lines both texts start with map one to one, the
        // lines both end with map with a shift, and the edit between them maps to nothing. two edits
        // far apart become one hunk spanning both: coarser than a diff, cheap (one pass over each
        // text), and every mapped line is the same bytes
        class LineMap
        {
        public:

            static LineMap build(const AST::File &compiled, const LiveText &live);

            // live == compiled: every line maps to itself
            static LineMap identity();

            bool identical() const {
                return _identity;
            }

            std::optional<uint32_t> to_snapshot(uint32_t live_line) const;
            std::optional<uint32_t> to_live(uint32_t snapshot_line) const;

            // a snapshot location on the live text, column unchanged: a mapped line is the same
            // bytes. nothing when its line is in the hunk
            std::optional<AST::Location> location_to_live(AST::Location snapshot_location) const;
            std::optional<AST::Location> location_to_snapshot(AST::Location live_location) const;

        private:

            bool _identity = false;
            uint32_t _prefix = 0;
            uint32_t _suffix = 0;
            uint32_t _live_lines = 0;
            uint32_t _snapshot_lines = 0;
        };

        // one request's view of the editor's text: a LiveText and LineMap per snapshot file it
        // touches, built once per file. a closed file reads as the snapshot
        class LiveViews
        {
        public:

            struct View
            {
                std::optional<LiveText> live;
                LineMap map = LineMap::identity();
            };

            explicit LiveViews(const Session &session);

            const View &view_of(const AST::File &file);

        private:

            const Session &_session;
            std::unordered_map<const AST::File *, View> _views;
        };

        std::optional<AST::Location> snapshot_location(
            LiveViews &views,
            const AST::File &file,
            Position position,
            bool utf8
        );
    };
};

#endif
