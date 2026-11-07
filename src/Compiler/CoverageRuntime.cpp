#include "Compiler/CoverageRuntime.h"

#include "Compiler/CBuild.h"
#include "Compiler/LinkRequirement.h"

#include "coverage_runtime_embedded.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{
    const Compiler::EmbeddedCFile k_files[] = {
        { "eco_coverage.h", k_coverage_runtime_header },
        { "eco_coverage.c", k_coverage_runtime_c },
    };
};

std::string Compiler::coverage_runtime_stamp()
{
    return embedded_c_stamp(k_files, sizeof(k_files) / sizeof(k_files[0]));
}

Compiler::LinkRequirement Compiler::coverage_runtime_link(const std::filesystem::path &object)
{
    return compiler_object_link(object, "--coverage");
}

bool Compiler::coverage_runtime_objects(
    const CompilerOptions &options,
    const std::filesystem::path &coverage_dir,
    const std::filesystem::path &scratch_dir,
    std::vector<LinkRequirement> &out_objects,
    std::string &out_error
)
{
    out_objects.clear();

    CBuildSpec spec;
    spec.module_name = "eco_coverage";
    spec.flags = { "-O2" };
    // UCRT deprecates getenv; clang prints that into echoc's stderr, which the
    // coverage goldens capture
    spec.defines = { "_CRT_SECURE_NO_WARNINGS" };

    std::vector<std::filesystem::path> objects;
    if (!compile_embedded_c(
            "the coverage runtime",
            spec,
            k_files,
            sizeof(k_files) / sizeof(k_files[0]),
            options,
            coverage_dir,
            scratch_dir,
            objects,
            out_error)) {
        return false;
    }

    for (const std::filesystem::path &object : objects) {
        out_objects.push_back(coverage_runtime_link(object));
    }

    return true;
}
