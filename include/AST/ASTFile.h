#ifndef ASTFILE_H
#define ASTFILE_H

#pragma once

#include "Token.h"
#include "AST/ASTImport.h"
#include "AST/ASTRegion.h"
#include "AST/ScopeNode.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AST
{
    class TypeAliasDeclNode;
    struct EnumMap;
    class Module;
    class File;

    class FileIterable
    {
        std::vector<std::unique_ptr<File>> &_files;
    public:
        FileIterable(std::vector<std::unique_ptr<File>> &files) : _files(files) {}

        class Iterator
        {
            typedef std::vector<std::unique_ptr<File>>::iterator VecIterator;
            VecIterator _it;
        public:
            Iterator(VecIterator it) : _it(it) {}
            Iterator &operator++() { ++_it; return *this; }
            bool operator!=(const Iterator &other) const { return _it != other._it; }
            File &operator*() const { return **_it; }
        };

        Iterator begin() { return Iterator(_files.begin()); }
        Iterator end() { return Iterator(_files.end()); }

        File *first() {
            if (_files.empty()) return nullptr;
            return _files.front().get();
        }
    };

    class File
    {
        const std::filesystem::path _path;

        // instead of storing to the content line by line for easy access
        // we will store a offset to the start of each line in the content
        std::vector<size_t> _line_offsets;

    public:

        Module *module = nullptr;

        ScopeNode *root = nullptr;

        // the file root is a region, the same unit ownership walks as a function body - codegen
        // synthesizes `main` out of it. t_ready is in-progress, t_owned is done. AST::RegionState
        RegionState region_state = RegionState::t_open;

        // file-local `use` aliases, in written order. AST::file_import_for is the reader.
        // **mutable** because TokenizedFile holds a const File* - the identity is settled at lex
        // time, the table is filled in pass 1 on that same object
        mutable std::vector<ImportBinding> imports;

        // named maps this file wrote. **mutable** for `imports`' reason. the owner enum may live
        // in another module, so mint_file_enum_maps cannot find these by walking this module's
        // TypeDeclNodes. unique_ptr so ComplexType can hold a stable EnumMap* for lookup
        mutable std::vector<std::unique_ptr<EnumMap>> enum_maps;

        // type aliases this file declares, minted in pass 1 and resolved before pass 2. **mutable** for
        // `imports`' reason
        mutable std::vector<TypeAliasDeclNode *> type_aliases;

        // the plain text content of the file
        std::optional<std::string> content;

        // this file exists only because a `#[target: test]` listed it. coverage skips it so a
        // helper in tests/ does not count as product code. a file also listed at module scope
        // stays false. stamped at File creation, which is what file_namespace_for reads
        bool tests_only = false;

        // where this file's file-scope declarations go when tests_only, null otherwise - the module's
        // private test namespace. `namespace a::b;` still means the program's `a::b`, and a lookup from
        // anywhere in the file searches this beside the root. **mutable** for `imports`' reason
        mutable Namespace *private_namespace = nullptr;


        // out of line: `enum_maps` holds unique_ptr to EnumMap, complete only in ASTFile.cpp
        File(const std::filesystem::path &path);
        ~File();

        const std::filesystem::path &get_path() const {
            return _path;
        }

        // sets the content of the file
        // this will also invalidate the line_offsets
        void set_content(const std::string &content);

        // set content from c string
        void set_content(const char *content, size_t length);

        // will read the file from disk and update the content
        void read_from_disk();

        // the same read, reporting *why* it failed. Prefer this wherever the path came from anywhere but a
        // command line the user just typed - see the note on the definition
        bool read_from_disk(std::string &out_error);

        std::string debug_description() const;

        std::string get_content_of_line(uint32_t line) const;

        // how many lines the content has. `get_content_of_line` answers "" for an empty line and for one
        // past the end, so a renderer drawing context rows around a span needs this to tell the two apart
        // - without it a diagnostic on the last line grows a blank row that is not in the file
        size_t line_count() const {
            return _line_offsets.size();
        }
    };

    struct TokenizedFile
    {
        const File *file;
        const TokenSlice token_slice;
    };

    // the private test namespace of the file `at` was written in, or null - the extra root frame an
    // unqualified lookup from there searches. null for a minted token, which names no file
    inline const Namespace *private_namespace_of(const TokenReference &at) {
        const File *file = at.is_valid() ? at.file() : nullptr;
        return file != nullptr ? file->private_namespace : nullptr;
    }
};
#endif
