#ifndef ARTIFACTFILE_H
#define ARTIFACTFILE_H

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Compiler
{
    // **the one way echoc writes a small file into a build directory.**
    // staged and renamed, never opened in place: a planted symlink would be written through.
    // `mode` is llvm::sys::fs permission bits; empty is the TempFile default (0666 before umask)
    bool write_artifact_file(
        const std::filesystem::path &path,
        std::string_view contents,
        std::optional<unsigned> mode = {}
    );

    // unlinks `path` (the link itself, not its target) before clang or a linker opens it in place
    void clear_artifact_path(const std::filesystem::path &path);

    // the whole file, or nullopt. **the content, never the timestamp**: a manifest's file list is a glob,
    // so a checkout, a branch switch or a `touch` all move mtimes without changing what is compiled
    std::optional<std::string> read_whole_file(const std::filesystem::path &path);
};

#endif
