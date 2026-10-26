#include "Compiler/ProfileRuntime.h"

#include "Compiler/CBuild.h"
#include "Compiler/LinkRequirement.h"

#include "profile_runtime_embedded.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{
    const Compiler::EmbeddedCFile k_files[] = {
        { "eco_profile.h", k_profile_runtime_header },
        { "eco_profile_internal.h", k_profile_runtime_internal },
        { "eco_profile.c", k_profile_runtime_c },
        { "eco_profile_intern.c", k_profile_runtime_intern },
        { "eco_profile_sample.c", k_profile_runtime_sample },
        { "eco_profile_alloc.c", k_profile_runtime_alloc },
        { "eco_profile_write.c", k_profile_runtime_write },
    };
};

std::string Compiler::profile_runtime_stamp()
{
    return embedded_c_stamp(k_files, sizeof(k_files) / sizeof(k_files[0]));
}

Compiler::LinkRequirement Compiler::profile_runtime_link(const std::filesystem::path &object)
{
    return compiler_object_link(object, "--profile");
}

bool Compiler::profile_runtime_objects(
    const CompilerOptions &options,
    const std::filesystem::path &profile_dir,
    const std::filesystem::path &scratch_dir,
    std::vector<LinkRequirement> &out_objects,
    std::string &out_error
)
{
    out_objects.clear();

    CBuildSpec spec;
    spec.module_name = "eco_profile";
    spec.flags = { "-O2", "-fno-omit-frame-pointer" };

    std::vector<std::filesystem::path> objects;
    if (!compile_embedded_c(
            "the profile runtime",
            spec,
            k_files,
            sizeof(k_files) / sizeof(k_files[0]),
            options,
            profile_dir,
            scratch_dir,
            objects,
            out_error)) {
        return false;
    }

    for (const std::filesystem::path &object : objects) {
        out_objects.push_back(profile_runtime_link(object));
    }

    return true;
}
