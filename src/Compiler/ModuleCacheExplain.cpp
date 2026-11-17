#include "Compiler/ModuleCache.h"

#include "Compiler/ArtifactFile.h"

#include <fmt/core.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <vector>

namespace
{

// the record's first word. A record without it is an older echoc's, which kept only the files
constexpr std::string_view k_inputs_record_version = "v2";

// one line of the record: `<tag> <first> <rest of the line>`, the rest being a path or a name that may hold
// spaces
struct RecordLine
{
    std::string first;
    std::string rest;
};

// what explain_miss says when no recorded part accounts for the miss
constexpr std::string_view k_unexplained_miss = "the compiler, target or build mode changed";

// `a=1;b=2;` as its entries
std::vector<std::string> configuration_entries(const std::string &configuration)
{
    std::vector<std::string> entries;
    size_t from = 0;

    while (from < configuration.size()) {
        const size_t end = configuration.find(';', from);
        const size_t stop = end == std::string::npos ? configuration.size() : end;

        if (stop > from) {
            entries.push_back(configuration.substr(from, stop - from));
        }

        from = stop + 1;
    }

    return entries;
}

// the part of `name=value` before the `=`
std::string entry_name(const std::string &entry)
{
    return entry.substr(0, entry.find('='));
}

std::string explain_configuration(
    const std::string &recorded,
    const std::vector<std::string> &now
)
{
    const std::vector<std::string> before = configuration_entries(recorded);

    for (const std::string &entry : now) {
        if (std::find(before.begin(), before.end(), entry) != before.end()) {
            continue;
        }

        // the same name with another value reads as one change rather than one new and one gone
        const std::string name = entry_name(entry);
        const bool repeatable = name == "define" || name == "target";
        auto was = std::find_if(before.begin(), before.end(), [&](const std::string &old) {
            return !repeatable && entry_name(old) == name;
        });

        if (was != before.end()) {
            return fmt::format("'{}' is now '{}'", *was, entry);
        }

        return fmt::format("it is now built with '{}'", entry);
    }

    for (const std::string &entry : before) {
        if (std::find(now.begin(), now.end(), entry) == now.end()) {
            return fmt::format("it is no longer built with '{}'", entry);
        }
    }

    return "";
}

// the three sentences of explain_digests, each formatted with the name
struct DigestWording
{
    std::string_view added;
    std::string_view changed;
    std::string_view gone;
};

// names with content digests, as recorded and as they are now. a name may have been recorded twice
std::string explain_digests(
    const std::map<std::string, std::set<std::string>> &recorded,
    const std::vector<std::pair<std::string, uint64_t>> &current,
    const DigestWording &wording
)
{
    std::set<std::string> current_names;

    for (const auto &[name, digest] : current) {
        current_names.insert(name);

        auto found = recorded.find(name);

        if (found == recorded.end()) {
            return fmt::format(fmt::runtime(wording.added), name);
        }

        if (found->second.count(Compiler::to_hex(digest)) == 0) {
            return fmt::format(fmt::runtime(wording.changed), name);
        }
    }

    for (const auto &[name, digests] : recorded) {
        if (current_names.count(name) == 0) {
            return fmt::format(fmt::runtime(wording.gone), name);
        }
    }

    return "";
}

// files, compared the way the key folds them: by file name and content, never by directory. A vendored copy
// at another path is the same input, and calling it new would blame the one thing that did not change
std::string explain_inputs(const std::vector<RecordLine> &recorded, const Compiler::ModuleCacheKey &key)
{
    std::map<std::string, std::set<std::string>> recorded_by_name;

    for (const RecordLine &line : recorded) {
        recorded_by_name[std::filesystem::path(line.rest).filename().string()].insert(line.first);
    }

    std::vector<std::pair<std::string, uint64_t>> current;

    for (const auto &[input, digest] : key.inputs) {
        current.emplace_back(input.filename().string(), digest);
    }

    return explain_digests(recorded_by_name, current, { "'{}' is new", "'{}' changed", "'{}' is gone" });
}

// modules by name with their keys: the declared dependencies, or every module parsed before this one
std::string explain_modules(
    const std::vector<RecordLine> &recorded,
    const std::vector<std::pair<std::string, std::string>> &current,
    bool is_dependency
)
{
    std::map<std::string, std::string> recorded_keys;

    for (const RecordLine &line : recorded) {
        recorded_keys.emplace(line.first, line.rest);
    }

    for (const auto &[name, hex] : current) {
        auto found = recorded_keys.find(name);

        if (found == recorded_keys.end()) {
            return is_dependency
                ? fmt::format("'{}' is a new dependency", name)
                : fmt::format("'{}' is now parsed before it", name);
        }

        if (found->second != hex) {
            return is_dependency
                ? fmt::format("its dependency '{}' changed", name)
                : fmt::format("'{}', parsed before it, changed", name);
        }
    }

    for (const RecordLine &line : recorded) {
        const bool still_there = std::any_of(current.begin(), current.end(), [&line](const auto &module) {
            return module.first == line.first;
        });

        if (!still_there) {
            return is_dependency
                ? fmt::format("'{}' is no longer a dependency", line.first)
                : fmt::format("'{}' is no longer parsed before it", line.first);
        }
    }

    // the same modules with the same keys, in another order: the running digest is order-sensitive because
    // what a module can see depends on what was parsed before it
    if (!is_dependency && recorded.size() == current.size()) {
        for (size_t i = 0; i < current.size(); ++i) {
            if (recorded[i].first != current[i].first) {
                return "the modules parsed before it are in another order";
            }
        }
    }

    return "";
}

std::string explain_conversions(const std::vector<RecordLine> &recorded, const Compiler::ModuleCacheKey &key)
{
    std::map<std::string, std::set<std::string>> recorded_by_name;

    for (const RecordLine &line : recorded) {
        recorded_by_name[line.rest].insert(line.first);
    }

    return explain_digests(recorded_by_name, key.conversions, {
        "'{}' publishes a conversion it can now see",
        "'{}', which publishes a conversion it can see, changed",
        "'{}', which published a conversion it could see, is gone",
    });
}

};

std::string Compiler::ModuleCacheKey::configuration_record(const std::vector<std::string> &entries)
{
    std::string text;

    for (const std::string &entry : entries) {
        text += entry;
        text += ';';
    }

    return text;
}

bool Compiler::write_inputs_record(const std::filesystem::path &path, const ModuleCacheKey &key)
{
    // the directory is the caller's - it prepared one before it decided to emit here at all, and creating
    // one on the way past would be a second place that answers where an artifact goes
    //
    // one tagged line per part of the key, in the order the key folds them
    std::string record = std::string(k_inputs_record_version) + " " + key.hex + "\n";
    record += "env " + to_hex(key.environment) + "\n";
    record += "config " + ModuleCacheKey::configuration_record(key.configuration) + "\n";

    for (const auto &[input, digest] : key.inputs) {
        record += "input " + to_hex(digest) + " " + input.string() + "\n";
    }

    for (const auto &[name, hex] : key.depends) {
        record += "dep " + name + " " + hex + "\n";
    }

    for (const auto &[name, hex] : key.preceding) {
        record += "before " + name + " " + hex + "\n";
    }

    for (const auto &[name, digest] : key.conversions) {
        record += "conversion " + to_hex(digest) + " " + name + "\n";
    }

    return write_artifact_file(path, record);
}

std::string Compiler::explain_miss(const std::filesystem::path &inputs_path, const ModuleCacheKey &key)
{
    std::ifstream in(inputs_path, std::ios::binary);
    if (!in) {
        return "";
    }

    std::string header;
    if (!std::getline(in, header)) {
        return "";
    }

    if (header.rfind(std::string(k_inputs_record_version) + " ", 0) != 0) {
        return "its record was written by an older echoc, which cannot say";
    }

    // what the earlier build recorded, by tag, in written order
    std::map<std::string, std::vector<RecordLine>> recorded;
    std::string line;

    while (std::getline(in, line)) {
        const size_t tag_end = line.find(' ');
        if (tag_end == std::string::npos) {
            continue;
        }

        const std::string tag = line.substr(0, tag_end);
        const std::string body = line.substr(tag_end + 1);

        // `config` is one value; every other tag is a first field and the rest of the line
        if (tag == "env" || tag == "config") {
            recorded[tag].push_back({ body, "" });
            continue;
        }

        const size_t first_end = body.find(' ');
        if (first_end == std::string::npos) {
            continue;
        }

        recorded[tag].push_back({ body.substr(0, first_end), body.substr(first_end + 1) });
    }

    // the environment first, since nothing below is comparable across two compilers; then the module's own
    // files, because a file is the reason a person can act on - and the record is one per module rather
    // than one per key, so a configuration it disagrees with may only be the *other* build's
    const std::vector<RecordLine> &env = recorded["env"];
    if (env.empty() || env.front().first != to_hex(key.environment)) {
        return std::string(k_unexplained_miss);
    }

    if (std::string why = explain_inputs(recorded["input"], key); !why.empty()) {
        return why;
    }

    const std::vector<RecordLine> &config = recorded["config"];
    const std::string recorded_config = config.empty() ? "" : config.front().first;
    if (std::string why = explain_configuration(recorded_config, key.configuration); !why.empty()) {
        return why;
    }

    // then the folds that are not this module's: a declared dependency before the running digest, so a
    // dependency that changed is named as one rather than as a module parsed before it

    if (std::string why = explain_modules(recorded["dep"], key.depends, true); !why.empty()) {
        return why;
    }

    if (std::string why = explain_modules(recorded["before"], key.preceding, false); !why.empty()) {
        return why;
    }

    if (std::string why = explain_conversions(recorded["conversion"], key); !why.empty()) {
        return why;
    }

    // every part agrees with the record, so what moved is something the record does not hold
    return std::string(k_unexplained_miss);
}
