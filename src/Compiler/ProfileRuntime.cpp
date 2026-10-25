#include "Compiler/ProfileRuntime.h"

#include "Compiler/ArtifactFile.h"
#include "Compiler/CBuild.h"
#include "Compiler/LinkRequirement.h"

#include "profile_runtime_embedded.h"

#include <fmt/core.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct EmbeddedFile
    {
        const char *name;
        const char *text;
    };

    const EmbeddedFile k_files[] = {
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
    std::string out;
    for (const EmbeddedFile &file : k_files) {
        out += file.text;
    }
    return out;
}

Compiler::LinkRequirement Compiler::profile_runtime_link(const std::filesystem::path &object)
{
    LinkRequirement requirement;
    requirement.scheme = LinkScheme::t_object;
    requirement.value = object.string();
    requirement.declared_by = "--profile";
    return requirement;
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

    std::error_code ec;
    std::filesystem::create_directories(profile_dir, ec);
    if (ec) {
        out_error = fmt::format("'{}' could not be created for the profile runtime.", profile_dir.string());
        return false;
    }

    CBuildSpec spec;
    spec.module_name = "eco_profile";
    spec.includes.push_back(std::filesystem::absolute(profile_dir));
    spec.flags = { "-O2", "-fno-omit-frame-pointer" };

    for (const EmbeddedFile &file : k_files) {
        const std::filesystem::path path = profile_dir / file.name;
        if (!write_artifact_file(path, file.text)) {
            out_error = "the profile runtime could not be written.";
            return false;
        }

        if (std::string_view(file.name).ends_with(".c")) {
            spec.sources.push_back(std::filesystem::absolute(path));
        }
    }

    std::vector<std::string> explain;
    CBuildResult compiled;
    const std::filesystem::path cache = profile_dir / "cc";

    if (!build_c_sources(spec, options, cache, scratch_dir, explain, compiled, out_error)) {
        return false;
    }

    if (compiled.objects.empty()) {
        out_error = "the profile runtime produced no object.";
        return false;
    }

    for (const std::filesystem::path &object : compiled.objects) {
        out_objects.push_back(profile_runtime_link(object));
    }

    return true;
}
