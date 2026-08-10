#include "Compiler/LinkRequirement.h"

#include "Compiler/SettledPath.h"

#include <fmt/core.h>

#include <algorithm>

namespace
{

// the scheme names, in the order the "expected one of" lists them. One array, read by the vocabulary
// question and by the message that rejects a value outside it
const std::vector<std::pair<std::string, Compiler::LinkScheme>> &scheme_table()
{
    static const std::vector<std::pair<std::string, Compiler::LinkScheme>> table = {
        { "lib", Compiler::LinkScheme::t_library },
        { "framework", Compiler::LinkScheme::t_framework },
        { "search", Compiler::LinkScheme::t_search },
        { "object", Compiler::LinkScheme::t_object },
    };

    return table;
}

};

std::string Compiler::link_scheme_list()
{
    return scheme_list_of(scheme_table());
}

std::string Compiler::link_requirement_spelling(const LinkRequirement &requirement)
{
    for (const auto &[spelled, scheme] : scheme_table()) {
        if (scheme == requirement.scheme) {
            return spelled + ":" + requirement.value;
        }
    }

    return requirement.value;
}

bool Compiler::parse_link_requirement(
    const std::string &spelled,
    const std::filesystem::path &base,
    const TargetFacts &facts,
    const std::string &declared_by,
    LinkRequirement &out,
    std::string &out_error
)
{
    LinkScheme scheme = LinkScheme::t_library;
    std::string value;

    if (!split_scheme(
            spelled, scheme_table(), "link scheme", link_scheme_list(), scheme, value, out_error)) {
        return false;
    }

    out = LinkRequirement{};
    out.scheme = scheme;
    out.declared_by = declared_by;

    switch (scheme) {
    case LinkScheme::t_library:
        out.value = value;
        return true;

    case LinkScheme::t_framework:
        // **refused rather than ignored.** A framework means nothing off Darwin, and quietly dropping it
        // would leave a linux build failing on the symbols it was supposed to provide with nothing saying
        // the declaration was never applied. Gating it is one line the author writes once
        if (facts.operating_system != "darwin") {
            out_error = fmt::format(
                "'framework:{}' is a Darwin framework and this build targets {}. Gate it with "
                "'#[if: os == darwin]' and name the platform's own library in the other arm.",
                value, facts.operating_system);
            return false;
        }

        out.value = value;
        return true;

    case LinkScheme::t_search: {
        const std::filesystem::path directory = settled_path(base, value);

        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec)) {
            out_error = fmt::format(
                "the search path '{}' resolves to '{}', which is not a directory.",
                value, directory.string());
            return false;
        }

        out.value = directory.string();
        return true;
    }

    case LinkScheme::t_object: {
        const std::filesystem::path object = settled_path(base, value);

        std::error_code ec;
        if (!std::filesystem::is_regular_file(object, ec)) {
            out_error = fmt::format(
                "the object '{}' resolves to '{}', which is not a file.", value, object.string());
            return false;
        }

        out.value = object.string();
        return true;
    }
    }

    return false;
}

void Compiler::partition_link_requirements(
    const std::vector<LinkRequirement> &requirements,
    std::vector<std::filesystem::path> &out_objects,
    std::vector<std::string> &out_words
)
{
    // search paths ahead of the libraries that need them: `-l` is resolved against the `-L`s already seen,
    // so a directory named after the library looking for it is a directory the linker never consults
    for (const LinkRequirement &requirement : requirements) {
        if (requirement.scheme == LinkScheme::t_object) {
            out_objects.push_back(requirement.value);
        }
        else if (requirement.scheme == LinkScheme::t_search) {
            out_words.push_back("-L" + requirement.value);
        }
    }

    for (const LinkRequirement &requirement : requirements) {
        switch (requirement.scheme) {
        case LinkScheme::t_library:
            out_words.push_back("-l" + requirement.value);
            break;

        case LinkScheme::t_framework:
            // two words, which is exactly what a flag string could not have carried through a dedup
            out_words.push_back("-framework");
            out_words.push_back(requirement.value);
            break;

        case LinkScheme::t_search:
        case LinkScheme::t_object:
            break;
        }
    }
}

std::optional<std::filesystem::path> Compiler::runtime_library_of(
    const LinkRequirement &requirement,
    const std::vector<LinkRequirement> &all,
    std::string &out_refusal
)
{
    switch (requirement.scheme) {
    case LinkScheme::t_search:
        // nothing to open; a search path is where the answer to another requirement lives
        return std::nullopt;

    case LinkScheme::t_object:
        out_refusal = fmt::format(
            "'object:{}' cannot be loaded by 'echoc run': the JIT resolves symbols out of the running "
            "process and an object file is not something it can open. Use 'echoc build' for this program, "
            "or ship the object as a library.",
            requirement.value);
        return std::nullopt;

    case LinkScheme::t_framework: {
        // the bundle's binary, which is what dlopen takes - the directory itself is not openable
        const std::filesystem::path bundle = std::filesystem::path("/System/Library/Frameworks")
            / (requirement.value + ".framework") / requirement.value;

        return bundle;
    }

    case LinkScheme::t_library: {
        // **the host's extension, deliberately.** `run` executes on this machine, so what `--target-os`
        // says a condition sees has nothing to do with what dlopen can open
        const std::string file_name =
            "lib" + requirement.value + TargetFacts::host().shared_library_extension();

        // **the declared search paths first, and only then the bare name.** dlopen knows nothing about the
        // `-L`s the link line carries, so a library that only exists under a `search:` directory would
        // link and then fail to load, which is the one failure that looks like the feature not working
        for (const LinkRequirement &other : all) {
            if (other.scheme != LinkScheme::t_search) {
                continue;
            }

            const std::filesystem::path candidate = std::filesystem::path(other.value) / file_name;

            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }

        // bare, for the loader to resolve the way it resolves everything else
        return std::filesystem::path(file_name);
    }
    }

    return std::nullopt;
}

void Compiler::merge_link_requirements(const std::vector<LinkRequirement> &incoming, std::vector<LinkRequirement> &into)
{
    for (const LinkRequirement &requirement : incoming) {
        if (std::find(into.begin(), into.end(), requirement) != into.end()) {
            continue;
        }

        into.push_back(requirement);
    }
}
