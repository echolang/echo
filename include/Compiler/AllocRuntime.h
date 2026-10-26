#ifndef ALLOCRUNTIME_H
#define ALLOCRUNTIME_H

#pragma once

#include "Compiler/CompilerOptions.h"
#include "Compiler/LinkRequirement.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Compiler
{
    // concatenated C source this echoc was configured with. folded into a
    // program stamp so an edited cache rebuilds
    std::string alloc_runtime_stamp();

    // keep eco_cache.o in the static archive so MCJIT can resolve it. the JIT
    // path calls this; the stamp does not
    void touch_alloc_runtime();

    // compile the cache into objects via build_c_sources. empty `out_objects` on failure
    bool alloc_runtime_objects(
        const CompilerOptions &options,
        const std::filesystem::path &alloc_dir,
        const std::filesystem::path &scratch_dir,
        std::vector<LinkRequirement> &out_objects,
        std::string &out_error
    );

    // `t_object` for the compiled cache, blamed on the runtime
    LinkRequirement alloc_runtime_link(const std::filesystem::path &object);

    // compile and append when `--alloc-cache` is on and the target is not wasm
    bool alloc_link_runtime(
        const CompilerOptions &options,
        const std::filesystem::path &alloc_dir,
        const std::filesystem::path &scratch_dir,
        std::vector<LinkRequirement> &objects,
        std::string &out_error
    );
};

#endif
