#ifndef SPAWNRUNTIME_H
#define SPAWNRUNTIME_H

#pragma once

#include "Compiler/CompilerOptions.h"
#include "Compiler/LinkRequirement.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Compiler
{
    std::string spawn_runtime_stamp();

    // keep eco_spawn.o in the archive, and name it for MCJIT: ELF echoc does
    // not export the symbol, so SearchForAddressOfSymbol would miss it
    void touch_spawn_runtime();

    bool spawn_runtime_objects(
        const CompilerOptions &options,
        const std::filesystem::path &spawn_dir,
        const std::filesystem::path &scratch_dir,
        std::vector<LinkRequirement> &out_objects,
        std::string &out_error
    );

    LinkRequirement spawn_runtime_link(const std::filesystem::path &object);

    bool spawn_link_runtime(
        const CompilerOptions &options,
        const std::filesystem::path &spawn_dir,
        const std::filesystem::path &scratch_dir,
        std::vector<LinkRequirement> &objects,
        std::string &out_error
    );
};

#endif
