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

            struct Entry
            {
                uint32_t line = 0;
                uint32_t column = 0;
                uint32_t width = 0;
                AST::Node *node = nullptr;
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
