#ifndef COVERAGERUNTIME_H
#define COVERAGERUNTIME_H

#pragma once

#include "Compiler/CompilerOptions.h"
#include "Compiler/LinkRequirement.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Compiler
{
    std::string coverage_runtime_stamp();

    bool coverage_runtime_objects(
        const CompilerOptions &options,
        const std::filesystem::path &coverage_dir,
        const std::filesystem::path &scratch_dir,
        std::vector<LinkRequirement> &out_objects,
        std::string &out_error
    );

    LinkRequirement coverage_runtime_link(const std::filesystem::path &object);
};

#endif
