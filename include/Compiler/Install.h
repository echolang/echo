#ifndef INSTALL_H
#define INSTALL_H

#pragma once

#include "Compiler/BuildLayout.h"
#include "Parser/ManifestParser.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Compiler
{
    // `--dir`, else `$ECHO_BIN_DIR`, else `~/.echo/bin` (`%LOCALAPPDATA%\echo\bin` on Windows)
    // false with a sentence rather than guessing at somebody's home
    //
    // a PATH directory, never a build dir: no CACHEDIR.TAG, and `echoc clean` cannot reach it
    // `create_directories` here is the one unmarked write the compiler is allowed
    bool resolve_install_dir(
        const std::filesystem::path &flag_directory,
        std::filesystem::path &out_directory,
        std::string &out_reason
    );

    // a sentence when `directory` is one `echoc clean` can empty, empty otherwise
    // asked before anything is built, so `--dir ecobuild` on a first install still refuses
    std::string install_dir_refusal(
        const std::filesystem::path &directory,
        const BuildLayout &layout,
        const std::vector<Parser::ModuleManifest> &manifests
    );

    // copy `built` into `directory` as `file_name`, creating the directory if it has to
    // false with a sentence
    //
    // stage then rename, never a write into the live file
    // writing in place reuses the inode, and a running (or already-signed) binary then dies
    // on the next launch instead of being replaced
    bool install_binary(
        const std::filesystem::path &built,
        const std::filesystem::path &directory,
        const std::filesystem::path &file_name,
        std::string &out_reason
    );

    // true when `directory` is one of `$PATH`'s entries
    bool directory_is_on_path(const std::filesystem::path &directory);
};

#endif
