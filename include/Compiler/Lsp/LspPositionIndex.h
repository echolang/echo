#ifndef LSPPOSITIONINDEX_H
#define LSPPOSITIONINDEX_H

#pragma once

#include "AST/ASTDiagnostic.h"
#include "AST/ASTNode.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>

namespace AST
{
    class Bundle;
    class File;
    class FunctionCallExprNode;
};

namespace Compiler
{
    namespace Lsp
    {
        // position → node, per AST::File. built by one RecursiveVisitor over each file root
        // after the semantic passes. the index holds Node* / File* into the bundle's arenas,
        // so it lives and dies with the Snapshot that owns both
        class PositionIndex
        {
        public:

            // what the token does to the thing it names. a highlight colours a write; an inlay
            // hint is owed only to a declaration
            enum class EntryRole : uint8_t
            {
                t_use,
                t_write,
                t_declaration,
                t_parameter
            };

            struct Entry
            {
                uint32_t line = 0;
                uint32_t column = 0;
                uint32_t width = 0;
                AST::Node *node = nullptr;
                EntryRole role = EntryRole::t_use;
            };

            struct CallSite
            {
                AST::FunctionCallExprNode *call = nullptr;
                AST::Span span;
            };

            void build(AST::Bundle &bundle);

            // 1-based byte coordinates, matching Token::line / Token::char_offset.
            // null when nothing contains the point
            const Entry *entry_at(const AST::File *file, uint32_t line, uint32_t column) const;
            AST::Node *at(const AST::File *file, uint32_t line, uint32_t column) const;

            const AST::File *file_for_path(const std::filesystem::path &path) const;

            // every entry of one file, sorted by position. empty when the file is unknown here
            const std::vector<Entry> &entries_of(const AST::File *file) const;

            // every written call of one file, with the span of its name and argument list
            const std::vector<CallSite> &calls_of(const AST::File *file) const;

            std::vector<const AST::File *> files() const;
            std::vector<std::string> paths() const;

            void visit_entries(
                const std::function<void(const AST::File &, const Entry &)> &fn
            ) const;

            // innermost written call whose name-plus-argument-list span contains the point
            AST::FunctionCallExprNode *enclosing_call(
                const AST::File *file,
                uint32_t line,
                uint32_t column
            ) const;

        private:

            std::unordered_map<const AST::File *, std::vector<Entry>> _by_file;
            std::unordered_map<const AST::File *, std::vector<CallSite>> _calls;
            std::unordered_map<std::string, const AST::File *> _by_path;
        };
    };
};

#endif
