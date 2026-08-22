#ifndef ASTMODULEEMBEDDER_H
#define ASTMODULEEMBEDDER_H

#pragma once

#include "AST/ASTModule.h"

#include <filesystem>
#include <string>

namespace AST
{
    // the path written into the embedded header for this file. a source under
    // STDLIB_SOURCE_DIR becomes `stdlib:<generic relative>`, so a Windows native
    // path cannot appear as a C string - `\ordered_map` is `\o{...}` and `\utf8`
    // is `\uXXXX`, both compile errors rather than a path
    std::string embedded_source_path(const std::filesystem::path &path);

    void write_embedded_module(AST::Module &module, const std::string &output_path);
};
#endif
