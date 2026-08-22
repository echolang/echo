#include "Compiler/LinkRequirement.h"

#include "Compiler/SettledPath.h"

#include <fmt/core.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

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

const std::vector<std::pair<std::string, Compiler::LinkLinkage>> &linkage_table()
{
    static const std::vector<std::pair<std::string, Compiler::LinkLinkage>> table = {
        { "dynamic", Compiler::LinkLinkage::t_dynamic },
        { "static", Compiler::LinkLinkage::t_static },
    };

    return table;
}

const std::vector<std::pair<std::string, Compiler::LinkRuntime>> &runtime_table()
{
    static const std::vector<std::pair<std::string, Compiler::LinkRuntime>> table = {
        { "load", Compiler::LinkRuntime::t_load },
        { "process", Compiler::LinkRuntime::t_process },
    };

    return table;
}

// the name a scheme is written under, in either medium
std::string scheme_name_of(Compiler::LinkScheme scheme)
{
    for (const auto &[spelled, candidate] : scheme_table()) {
        if (candidate == scheme) {
            return spelled;
        }
    }

    return "";
}

template <class Scheme>
bool name_in_table(
    const std::string &spelled,
    const std::vector<std::pair<std::string, Scheme>> &table,
    Scheme &out
)
{
    for (const auto &[candidate, scheme] : table) {
        if (candidate == spelled) {
            out = scheme;
            return true;
        }
    }

    return false;
}

// what a scheme and a value *mean*, once the two have been read - which is the whole of what the two
// spellings share. A manifest's tag and a command line's `<scheme>:` differ in how the pair is spelled
// and in nothing after it, so the platform refusal and the two path settlements live here rather than
// once per entry point
bool settle_link_requirement(
    Compiler::LinkScheme scheme,
    const std::string &value,
    const std::filesystem::path &base,
    const Compiler::TargetFacts &facts,
    const std::string &declared_by,
    Compiler::LinkRequirement &out,
    std::string &out_error
)
{
    out = Compiler::LinkRequirement{};
    out.scheme = scheme;
    out.declared_by = declared_by;

    // the value as written, so a refusal below has a requirement worth quoting back. The two path schemes
    // replace it with what it settled to once they have proved the path is there
    out.value = value;

    switch (scheme) {
    case Compiler::LinkScheme::t_library:
        return true;

    case Compiler::LinkScheme::t_framework:
        // **refused rather than ignored.** A framework means nothing off Darwin, and quietly dropping it
        // would leave a linux build failing on the symbols it was supposed to provide with nothing saying
        // the declaration was never applied. Gating it is one line the author writes once
        if (facts.operating_system != "darwin") {
            out_error = fmt::format(
                "'{}' is a Darwin framework and this build targets {}. Gate it with "
                "'#[if: os == darwin]' and name the platform's own library in the other arm.",
                Compiler::link_requirement_spelling(out), facts.operating_system);
            return false;
        }

        return true;

    case Compiler::LinkScheme::t_search: {
        const std::filesystem::path directory = Compiler::settled_path(base, value);

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

    case Compiler::LinkScheme::t_object: {
        const std::filesystem::path object = Compiler::settled_path(base, value);

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

// directories the host loader already searches. `search:` is consulted first and is not in this
// list - that is a declared path, not a platform default. Windows resolves against PATH via the
// bare name, so it contributes nothing here
std::vector<std::filesystem::path> host_library_directories()
{
    const std::string &os = Compiler::TargetFacts::host().operating_system;

    std::vector<std::filesystem::path> directories;

    if (os == "linux") {
        directories = {
            "/lib",
            "/usr/lib",
            "/lib64",
            "/usr/lib64",
            "/lib/x86_64-linux-gnu",
            "/usr/lib/x86_64-linux-gnu",
            "/lib/aarch64-linux-gnu",
            "/usr/lib/aarch64-linux-gnu",
        };
    }
    else if (os == "darwin") {
        directories = { "/usr/lib", "/usr/local/lib", "/opt/homebrew/lib" };
    }

    std::vector<std::filesystem::path> existing;
    existing.reserve(directories.size());

    for (const std::filesystem::path &directory : directories) {
        std::error_code ec;
        if (std::filesystem::is_directory(directory, ec)) {
            existing.push_back(directory);
        }
    }

    return existing;
}

// loadable files in `directory` for this name. Zero, one, or several: several is the caller's
// refusal, not a ranking. An unversioned DSO, when it is a real one, is the only answer - the
// versioned neighbours sit beside a linker script, not beside a second winner
std::vector<std::filesystem::path> loadables_in_directory(
    const std::filesystem::path &directory,
    const std::string &name,
    const std::optional<std::string> &file
)
{
    std::vector<std::filesystem::path> found;

    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return found;
    }

    if (file.has_value()) {
        const std::filesystem::path candidate = directory / file.value();

        if (Compiler::is_loadable_shared_object(candidate)) {
            found.push_back(candidate);
        }

        return found;
    }

    const std::string extension = Compiler::TargetFacts::host().shared_library_extension();
    const std::string unversioned = "lib" + name + extension;
    const std::filesystem::path exact = directory / unversioned;

    if (Compiler::is_loadable_shared_object(exact)) {
        found.push_back(exact);
        return found;
    }

    const std::string prefix = unversioned + ".";

    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(
             directory, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            break;
        }

        const std::string filename = entry.path().filename().string();

        if (filename.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }

        if (!Compiler::is_loadable_shared_object(entry.path())) {
            continue;
        }

        found.push_back(entry.path());
    }

    return found;
}

// a `lib<name>.a` / `lib<name>.lib` sitting in a declared search directory, or nullopt. The one
// lookup the static arm of the renderer is allowed - it does not walk the host and it does not
// invent flags
std::optional<std::filesystem::path> archive_in_search(
    const std::string &name,
    const std::vector<Compiler::LinkRequirement> &requirements
)
{
    for (const Compiler::LinkRequirement &other : requirements) {
        if (other.scheme != Compiler::LinkScheme::t_search) {
            continue;
        }

        const std::filesystem::path directory(other.value);
        const std::string candidates[] = {
            "lib" + name + ".a",
            "lib" + name + ".lib",
            name + ".lib",
            "lib" + name + "_a.lib",
        };

        std::error_code ec;
        for (const std::string &filename : candidates) {
            const std::filesystem::path archive = directory / filename;
            if (std::filesystem::is_regular_file(archive, ec)) {
                return archive;
            }
        }
    }

    return std::nullopt;
}

std::string asker_of_requirement(const std::string &declared_by)
{
    return declared_by.empty()
        ? std::string("the command line")
        : fmt::format("module '{}'", declared_by);
}

bool parse_lib_record(
    const AST::AttributeValue &record,
    Compiler::LinkScheme scheme,
    const std::filesystem::path &base,
    const Compiler::TargetFacts &facts,
    const std::string &declared_by,
    AST::AttributeReader &reader,
    Compiler::LinkRequirement &out
)
{
    reader.reject_unknown_fields(record, { "name", "linkage", "runtime", "file" });

    const AST::AttributeValue *name_value = reader.require_field(record, "name");

    if (name_value == nullptr) {
        return false;
    }

    std::optional<std::string> name = reader.string(*name_value);

    if (!name.has_value()) {
        return false;
    }

    Compiler::LinkLinkage linkage = Compiler::LinkLinkage::t_dynamic;
    Compiler::LinkRuntime runtime = Compiler::LinkRuntime::t_load;
    std::optional<std::string> file;

    if (const AST::AttributeValue *written = reader.field(record, "linkage")) {
        std::optional<std::string> spelled = reader.name(*written);

        if (!spelled.has_value()) {
            return false;
        }

        if (!name_in_table(spelled.value(), linkage_table(), linkage)) {
            reader.refuse(written->span, fmt::format(
                "'{}' is not a linkage, expected one of: {}.",
                spelled.value(), Compiler::scheme_list_of(linkage_table())));
            return false;
        }
    }

    if (const AST::AttributeValue *written = reader.field(record, "runtime")) {
        std::optional<std::string> spelled = reader.name(*written);

        if (!spelled.has_value()) {
            return false;
        }

        if (!name_in_table(spelled.value(), runtime_table(), runtime)) {
            reader.refuse(written->span, fmt::format(
                "'{}' is not a runtime, expected one of: {}.",
                spelled.value(), Compiler::scheme_list_of(runtime_table())));
            return false;
        }
    }

    if (const AST::AttributeValue *written = reader.field(record, "file")) {
        file = reader.string(*written);

        if (!file.has_value()) {
            return false;
        }
    }

    std::string reason;

    if (!settle_link_requirement(scheme, name.value(), base, facts, declared_by, out, reason)) {
        reader.refuse(record.span, reason);
        return false;
    }

    out.linkage = linkage;
    out.runtime = runtime;
    out.file = file;
    return true;
}

};

std::string Compiler::link_scheme_list()
{
    return scheme_list_of(scheme_table());
}

std::string Compiler::link_requirement_spelling(const LinkRequirement &requirement)
{
    const std::string name = scheme_name_of(requirement.scheme);

    if (name.empty()) {
        return requirement.value;
    }

    const bool as_record = requirement.scheme == LinkScheme::t_library
        && (requirement.linkage != LinkLinkage::t_dynamic
            || requirement.runtime != LinkRuntime::t_load
            || requirement.file.has_value());

    if (as_record) {
        std::string body = fmt::format("name: \"{}\"", requirement.value);

        if (requirement.linkage == LinkLinkage::t_static) {
            body += ", linkage: static";
        }

        if (requirement.runtime == LinkRuntime::t_process) {
            body += ", runtime: process";
        }

        if (requirement.file.has_value()) {
            body += fmt::format(", file: \"{}\"", requirement.file.value());
        }

        return name + " { " + body + " }";
    }

    // `declared_by` is already the record of which medium wrote it: a manifest credits its module, a
    // command line credits nobody
    return requirement.declared_by.empty()
        ? name + ":" + requirement.value
        : fmt::format("{} \"{}\"", name, requirement.value);
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

    return settle_link_requirement(scheme, value, base, facts, declared_by, out, out_error);
}

bool Compiler::parse_link_attribute(
    const AST::AttributeValue &value,
    const std::filesystem::path &base,
    const TargetFacts &facts,
    const std::string &declared_by,
    AST::AttributeReader &reader,
    std::vector<LinkRequirement> &out
)
{
    bool settled_all = true;

    // two peels, and they answer different questions. the outer one fans out an *untagged* list, where
    // each item carries its own scheme - `[lib "GL", framework "OpenGL"]`. the inner one fans out the
    // payload of a scheme already resolved - `lib ["GL", "GLU"]`, which is one scheme and two libraries
    for (const AST::AttributeValue *entry : AST::AttributeReader::each(value)) {
        LinkScheme scheme = LinkScheme::t_library;

        if (!reader.tag(*entry, scheme_table(), "link scheme", link_scheme_list(), scheme)) {
            settled_all = false;
            continue;
        }

        if (entry->is(AST::AttributeValueKind::t_record)) {
            if (scheme != LinkScheme::t_library) {
                reader.refuse(entry->span, fmt::format(
                    "a record payload is a '{}' shape - write '{} \"...\"' for this scheme.",
                    scheme_name_of(LinkScheme::t_library), scheme_name_of(scheme)));
                settled_all = false;
                continue;
            }

            LinkRequirement requirement;

            if (!parse_lib_record(*entry, scheme, base, facts, declared_by, reader, requirement)) {
                settled_all = false;
                continue;
            }

            out.push_back(requirement);
            continue;
        }

        for (const AST::AttributeValue *word : AST::AttributeReader::payload(*entry)) {
            std::optional<std::string> spelled = reader.string(*word);

            if (!spelled.has_value()) {
                settled_all = false;
                continue;
            }

            LinkRequirement requirement;
            std::string reason;

            if (!settle_link_requirement(
                    scheme, spelled.value(), base, facts, declared_by, requirement, reason)) {
                reader.refuse(word->span, reason);
                settled_all = false;
                continue;
            }

            out.push_back(requirement);
        }
    }

    return settled_all;
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
            if (requirement.linkage == LinkLinkage::t_static) {
                if (const auto archive = archive_in_search(requirement.value, requirements)) {
                    out_objects.push_back(archive.value());
                    break;
                }
            }

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

bool Compiler::is_loadable_shared_object(const std::filesystem::path &file)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return false;
    }

    std::ifstream in(file, std::ios::binary);
    unsigned char magic[4] = {};
    in.read(reinterpret_cast<char *>(magic), 4);

    if (!in || in.gcount() < 4) {
        return false;
    }

    if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
        return true;
    }

    if (magic[0] == 'M' && magic[1] == 'Z') {
        return true;
    }

    static const unsigned char mach_o[][4] = {
        { 0xfe, 0xed, 0xfa, 0xce },
        { 0xce, 0xfa, 0xed, 0xfe },
        { 0xfe, 0xed, 0xfa, 0xcf },
        { 0xcf, 0xfa, 0xed, 0xfe },
        { 0xca, 0xfe, 0xba, 0xbe },
        { 0xbe, 0xba, 0xfe, 0xca },
        { 0xca, 0xfe, 0xba, 0xbf },
        { 0xbf, 0xba, 0xfe, 0xca },
    };

    for (const unsigned char (&signature)[4] : mach_o) {
        if (std::memcmp(magic, signature, 4) == 0) {
            return true;
        }
    }

    return false;
}

std::optional<std::filesystem::path> Compiler::find_loadable_library(
    const std::string &name,
    const std::vector<std::filesystem::path> &search_dirs,
    const std::optional<std::string> &file,
    std::string &out_refusal
)
{
    const auto consider = [&](const std::filesystem::path &directory) -> std::optional<std::filesystem::path> {
        const std::vector<std::filesystem::path> found = loadables_in_directory(directory, name, file);

        if (found.size() == 1) {
            return found.front();
        }

        if (found.size() > 1) {
            std::string names;

            for (const std::filesystem::path &path : found) {
                names += names.empty() ? path.filename().string() : ", " + path.filename().string();
            }

            out_refusal = fmt::format(
                "several loadable files match '{}' in '{}': {}. Write 'file:' to name one.",
                file.value_or("lib" + name + TargetFacts::host().shared_library_extension()),
                directory.string(),
                names);
        }

        return std::nullopt;
    };

    for (const std::filesystem::path &directory : search_dirs) {
        if (const auto found = consider(directory)) {
            return found;
        }

        if (!out_refusal.empty()) {
            return std::nullopt;
        }
    }

    for (const std::filesystem::path &directory : host_library_directories()) {
        if (const auto found = consider(directory)) {
            return found;
        }

        if (!out_refusal.empty()) {
            return std::nullopt;
        }
    }

    return std::nullopt;
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
            "'{}' cannot be loaded by 'echoc run': the JIT resolves symbols out of the running "
            "process and an object file is not something it can open. Use 'echoc build' for this program, "
            "or ship the object as a library.",
            link_requirement_spelling(requirement));
        return std::nullopt;

    case LinkScheme::t_framework: {
        // the bundle's binary, which is what dlopen takes - the directory itself is not openable
        const std::filesystem::path bundle = std::filesystem::path("/System/Library/Frameworks")
            / (requirement.value + ".framework") / requirement.value;

        return bundle;
    }

    case LinkScheme::t_library: {
        if (requirement.runtime == LinkRuntime::t_process) {
            return std::nullopt;
        }

        if (requirement.linkage == LinkLinkage::t_static) {
            out_refusal = fmt::format(
                "'{}' cannot be loaded by 'echoc run': a static library is an archive, and the JIT "
                "resolves symbols out of the running process. Use 'echoc build' for this program, "
                "or ship the library as a shared object.",
                link_requirement_spelling(requirement));
            return std::nullopt;
        }

        std::vector<std::filesystem::path> search_dirs;

        for (const LinkRequirement &other : all) {
            if (other.scheme == LinkScheme::t_search) {
                search_dirs.emplace_back(other.value);
            }
        }

        if (const auto found =
                find_loadable_library(requirement.value, search_dirs, requirement.file, out_refusal)) {
            return found;
        }

        if (!out_refusal.empty()) {
            return std::nullopt;
        }

        // bare, for the loader to resolve the way it resolves everything else - Darwin's dyld cache
        // still answers `libpthread.dylib` with no file on disk
        const std::string file_name = requirement.file.value_or(
            "lib" + requirement.value + TargetFacts::host().shared_library_extension());

        return std::filesystem::path(file_name);
    }
    }

    return std::nullopt;
}

bool Compiler::merge_link_requirements(
    const std::vector<LinkRequirement> &incoming,
    std::vector<LinkRequirement> &into,
    std::string &out_error
)
{
    for (const LinkRequirement &requirement : incoming) {
        const auto existing = std::find(into.begin(), into.end(), requirement);

        if (existing == into.end()) {
            into.push_back(requirement);
            continue;
        }

        if (existing->linkage == requirement.linkage
            && existing->runtime == requirement.runtime
            && existing->file == requirement.file) {
            continue;
        }

        out_error = fmt::format(
            "'{}' asked for by {} disagrees with '{}' asked for by {} - the same library cannot "
            "be linked two ways.",
            link_requirement_spelling(requirement),
            asker_of_requirement(requirement.declared_by),
            link_requirement_spelling(*existing),
            asker_of_requirement(existing->declared_by));
        return false;
    }

    return true;
}
