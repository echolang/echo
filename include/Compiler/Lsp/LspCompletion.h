#ifndef LSPCOMPLETION_H
#define LSPCOMPLETION_H

#pragma once

#include "Compiler/Lsp/LspCompletionContext.h"
#include "Compiler/Lsp/LspLiveText.h"
#include "Compiler/Lsp/LspSnapshot.h"

#include <cstddef>
#include <string>
#include <vector>

namespace AST
{
    class File;
};

namespace Compiler
{
    namespace Lsp
    {
        // the CompletionItemKind numbers
        enum class CompletionItemKind
        {
            t_method = 2,
            t_function = 3,
            t_field = 5,
            t_variable = 6,
            t_class = 7,
            t_interface = 8,
            t_module = 9,
            t_property = 10,
            t_enum = 13,
            t_keyword = 14,
            t_enum_member = 20,
            t_constant = 21,
            t_struct = 22
        };

        struct CompletionItem
        {
            std::string label;
            CompletionItemKind kind = CompletionItemKind::t_keyword;

            // the whole signature or type, for a client that shows only this
            std::string detail;

            // `(int32 $a)` right after the label. the return type, value type, or declaring
            // namespace sits right-aligned. LSP labelDetails
            std::string label_detail;
            std::string label_description;

            std::string sort_text;
            std::string filter_text;
            std::string insert_text;

            // insert_text is a snippet: `push($0)`. an Echo `$` would be a snippet placeholder,
            // so the insert text leaves it off
            bool is_snippet = false;
        };

        struct CompletionAnswer
        {
            std::vector<CompletionItem> items;

            // live byte offsets. an item replaces start..cursor, or start..end when the editor
            // offers to replace the rest of the name under the cursor
            size_t replace_start = 0;
            size_t replace_end = 0;
        };

        struct CompletionRequest
        {
            // both null before the first compile. the answer is then the keywords and the variables
            // the text declares, which is still most of what is typed
            const Snapshot *snapshot = nullptr;
            const AST::File *file = nullptr;

            const LiveText *live = nullptr;
            LineMap map = LineMap::identity();
            size_t cursor = 0;

            bool snippets = false;
            bool is_manifest = false;

            // the editor asked because a trigger character was typed. only `->`, `::`, and a
            // shorthand `.` start a list; a lone `>`, `:`, or `.` answers empty
            bool from_trigger_character = false;
        };

        CompletionAnswer completion(const CompletionRequest &request);
    };
};

#endif
