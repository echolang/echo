#ifndef LSPURI_H
#define LSPURI_H

#pragma once

#include <filesystem>
#include <string>

namespace Compiler
{
    namespace Lsp
    {
        // file:// URI ↔ path, with percent-encoding. one owner so a diagnostic and a
        // go-to-definition cannot disagree about what a path is called
        std::filesystem::path path_from_uri(const std::string &uri);
        std::string uri_from_path(const std::filesystem::path &path);

        // the embedded stdlib's virtual `stdlib:` paths. a client cannot open them, so
        // definition and references refuse them rather than sending the editor hunting
        bool is_embedded_stdlib_path(const std::filesystem::path &path);
    };
};

#endif
