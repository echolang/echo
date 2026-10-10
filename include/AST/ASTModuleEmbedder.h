#ifndef ASTMODULEEMBEDDER_H
#define ASTMODULEEMBEDDER_H

#pragma once

#include "AST/ASTModule.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace AST
{
    // the path written into the embedded header for this file. a source under
    // STDLIB_SOURCE_DIR becomes `stdlib:<generic relative>`, so a Windows native
    // path cannot appear as a C string - `\ordered_map` is `\o{...}` and `\utf8`
    // is `\uXXXX`, both compile errors rather than a path
    std::string embedded_source_path(const std::filesystem::path &path);

    // relative posix path from the stdlib root (`core/array.eco`). empty when
    // `path` is not under STDLIB_SOURCE_DIR
    std::string embedded_relative_path(const std::filesystem::path &path);

    // every `.eco` file under the stdlib root except `build/` and `sketches/`,
    // plus `module.eco`. relative posix path, then bytes. the set a released
    // echoc embeds, so `#[if:]` on the manifest can still drop files per target
    std::vector<std::pair<std::string, std::string>> collect_stdlib_files(const std::filesystem::path &stdlib_root);

    // test-only: emits a `load_<name>_module` that includes AST headers. the
    // production header is write_embedded_stdlib
    void write_embedded_module(AST::Module &module, const std::string &output_path);

    // walk `stdlib_root` and emit the table `ParsePipeline` loads under
    // `ECO_USE_EMBEDDED_STDLIB`. includes `module.eco` and every source the
    // manifest might name, gated or not. the generated header includes only
    // cstddef / filesystem / optional / string / string_view / vector
    void write_embedded_stdlib(const std::filesystem::path &stdlib_root, const std::string &output_path);
};
#endif
