#ifndef PROFILERUNTIME_H
#define PROFILERUNTIME_H

#pragma once

#include "Compiler/CompilerOptions.h"
#include "Compiler/LinkRequirement.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Compiler
{
    // concatenated C sources + headers this echoc was configured with.
    // folded into a program stamp so an edited runtime rebuilds
    std::string profile_runtime_stamp();

    // compile the runtime into objects via build_c_sources. empty `out_objects` on failure
    bool profile_runtime_objects(
        const CompilerOptions &options,
        const std::filesystem::path &profile_dir,
        const std::filesystem::path &scratch_dir,
        std::vector<LinkRequirement> &out_objects,
        std::string &out_error
    );

    // `t_object` for one compiled runtime piece, blamed on --profile
    LinkRequirement profile_runtime_link(const std::filesystem::path &object);
};

#endif
