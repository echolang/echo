#ifndef ASTIMPORT_H
#define ASTIMPORT_H

#pragma once

#include "Token.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AST
{
    class Collector;
    class File;
    class Namespace;
    struct Context;

    // what a file-local `use` bound a name to. an alias, not a declaration: nothing is published
    // into any namespace, and a second file of the same namespace does not see it
    enum class ImportKind
    {
        t_unresolved,
        t_namespace,
        t_item,
    };

    struct ImportBinding
    {
        ImportKind kind = ImportKind::t_unresolved;

        // the path as written, group prefix included. `use std::math::sqrt` is
        // {"std", "math", "sqrt"}
        std::vector<std::string> path;

        // the name this file writes. the last segment, or the `as` name
        std::string local_name;

        // an item's real name, the one the registry and the namespace table know
        std::string target_name;

        // a prefix: the namespace itself. an item: the namespace that holds the name
        Namespace *target_namespace = nullptr;

        std::optional<TokenReference> local_token;
        std::optional<TokenSlice> span;

        bool reported = false;
    };

    // the file the parser is walking. TokenizedFile holds a const File*; File::imports is
    // mutable so pass 1 can fill the table without punching a hole in that const
    const File &file_of(const Context &context);

    // **the one reader of a file's import table.** classifies a still-unresolved binding on
    // demand (types and namespaces may already exist; functions and constants of this module
    // may not). null when this file did not bind `name`, or the binding is still unresolved.
    // every unqualified lookup that a `use` may rewrite goes through here rather than walking
    // File::imports itself, so a second table cannot grow beside it
    const ImportBinding *file_import_for(const File &file, Collector &collector, std::string_view name);

    // an item import only. null when `name` is unbound, still unresolved, or a prefix
    const ImportBinding *item_import_for(const File &file, Collector &collector, std::string_view name);

    // the namespace a leading identifier should start from after a `use`: the bound namespace
    // itself for a prefix, the item's own namespace for an item. null when this file did not
    // bind `name` that way
    Namespace *imported_namespace_start(
        const File &file,
        Collector &collector,
        std::string_view name
    );

    // a written `a::b::c` prefix, first segment applied as a `use`. parse_namespace mints
    // missing children so a later lookup is exact; parse_static_owner does not, so a name
    // that is a type stays silent for the type path to try. empty parts answers null
    Namespace *namespace_from_written_path(
        const File &file,
        Collector &collector,
        const std::vector<std::string> &parts,
        bool mint
    );

    // retarget an unqualified name this file imported as an item. true when it did: `ns` is
    // the item's namespace, `imported_name` is the registry name when `as` renamed it
    bool apply_item_import(
        const File &file,
        Collector &collector,
        std::string_view written,
        const Namespace *&ns,
        std::string &imported_name);

    // refuse any binding still unknown. after pass 2 everything this module declares exists,
    // so a path that is still unresolved is an error at the `use`, not at every call
    void finalize_file_imports(const File &file, Collector &collector);

    std::string join_namespace_path(const std::vector<std::string> &parts);
};

#endif
