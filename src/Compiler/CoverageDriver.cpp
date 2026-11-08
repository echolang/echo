#include "Compiler/CoverageDriver.h"

#include "Compiler/ArtifactFile.h"
#include "Compiler/CoverageRuntime.h"
#include "Compiler/Fnv.h"
#include "Compiler/TestSelection.h"

#include <eco_coverage.h>

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <vector>

const char *Compiler::coverage_site_kind_name(CoverageSiteKind kind)
{
    switch (kind) {
    case CoverageSiteKind::t_fn:
        return "fn";
    case CoverageSiteKind::t_line:
        return "line";
    case CoverageSiteKind::t_branch:
        return "branch";
    }

    return "line";
}

std::string Compiler::coverage_module_stem(std::string_view name)
{
    std::string out;
    out.reserve(name.size());
    for (char ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z')
            || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9')
            || ch == '_';
        out.push_back(ok ? ch : '_');
    }

    if (out.empty()) {
        out = "m";
    }

    const uint64_t digest = fnv1a64(name.data(), name.size(), k_fnv_offset_basis);
    return fmt::format("{}_{}", out, to_hex(digest).substr(0, 8));
}

bool Compiler::coverage_link_runtime(
    const CompilerOptions &options,
    const BuildLayout &layout,
    std::vector<LinkRequirement> &objects,
    std::string &out_error
)
{
    if (!options.coverage()) {
        return true;
    }

    std::vector<LinkRequirement> runtime;
    if (!coverage_runtime_objects(
            options,
            layout.scratch_dir() / "coverage",
            layout.scratch_dir(),
            runtime,
            out_error)) {
        return false;
    }

    objects.insert(objects.end(), runtime.begin(), runtime.end());
    return true;
}

std::filesystem::path Compiler::coverage_map_path(const BuildLayout &layout)
{
    return layout.coverage_dir() / "map.json";
}

std::filesystem::path Compiler::coverage_object_map_path(const std::filesystem::path &object)
{
    return object.parent_path() / (object.stem().string() + ".covmap.json");
}

std::filesystem::path Compiler::coverage_test_path(const BuildLayout &layout, const TestCase &test)
{
    std::string id = test_display_name(test);
    for (char &ch : id) {
        const bool ok = (ch >= 'a' && ch <= 'z')
            || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9')
            || ch == '_' || ch == '-' || ch == '.';
        if (!ok) {
            ch = '_';
        }
    }

    return layout.coverage_dir() / "tests" / (id + ".ecocov");
}

namespace
{
    std::filesystem::path tests_dir(const Compiler::BuildLayout &layout)
    {
        return layout.coverage_dir() / "tests";
    }

    Compiler::CoverageSiteKind kind_from_name(const std::string &name)
    {
        if (name == "fn") {
            return Compiler::CoverageSiteKind::t_fn;
        }
        if (name == "branch") {
            return Compiler::CoverageSiteKind::t_branch;
        }
        return Compiler::CoverageSiteKind::t_line;
    }

    nlohmann::ordered_json counters_json(const std::vector<Compiler::CoverageSite> &sites)
    {
        nlohmann::ordered_json counters = nlohmann::ordered_json::array();
        for (const Compiler::CoverageSite &site : sites) {
            nlohmann::ordered_json row;
            row["local"] = site.id;
            row["file"] = site.file;
            row["line"] = site.line;
            row["column"] = site.column;
            row["kind"] = Compiler::coverage_site_kind_name(site.kind);
            if (!site.edge.empty()) {
                row["edge"] = site.edge;
            }
            row["function"] = site.function;
            counters.push_back(std::move(row));
        }
        return counters;
    }

    std::optional<std::vector<Compiler::CoverageSite>> sites_from_json(const nlohmann::json &module)
    {
        if (!module.contains("counters") || !module["counters"].is_array()) {
            return std::nullopt;
        }

        std::vector<Compiler::CoverageSite> sites;
        const std::string name = module.value("name", "");
        for (const nlohmann::json &row : module["counters"]) {
            Compiler::CoverageSite site;
            site.id = row.value("local", 0u);
            site.module = name;
            site.dir = module.value("dir", "");
            site.file = row.value("file", "");
            site.line = row.value("line", 0u);
            site.column = row.value("column", 0u);
            site.kind = kind_from_name(row.value("kind", ""));
            site.edge = row.value("edge", "");
            site.function = row.value("function", "");
            sites.push_back(std::move(site));
        }
        return sites;
    }

    uint64_t hash_sites(const Compiler::CoverageModuleSites &modules)
    {
        uint64_t hash = Compiler::k_fnv_offset_basis;
        for (const auto &[name, sites] : modules) {
            hash = Compiler::fnv1a64(name, hash);
            const uint64_t n = sites.size();
            hash = Compiler::fnv1a64(reinterpret_cast<const char *>(&n), sizeof(n), hash);
            for (const Compiler::CoverageSite &site : sites) {
                hash = Compiler::fnv1a64(site.dir, hash);
                hash = Compiler::fnv1a64(site.file, hash);
                hash = Compiler::fnv1a64(Compiler::coverage_site_kind_name(site.kind), hash);
                hash = Compiler::fnv1a64(site.edge, hash);
                hash = Compiler::fnv1a64(site.function, hash);
                hash = Compiler::fnv1a64(
                    reinterpret_cast<const char *>(&site.line), sizeof(site.line), hash);
            }
        }
        return hash;
    }

    std::string map_json_of(const Compiler::CoverageModuleSites &modules)
    {
        nlohmann::ordered_json json;
        json["v"] = 2;
        json["hash"] = Compiler::to_hex(hash_sites(modules));

        nlohmann::ordered_json rows = nlohmann::ordered_json::array();
        for (const auto &[name, sites] : modules) {
            nlohmann::ordered_json module;
            module["name"] = name;
            if (!sites.empty() && !sites.front().dir.empty()) {
                module["dir"] = sites.front().dir;
            }
            module["counters"] = counters_json(sites);
            rows.push_back(std::move(module));
        }
        json["modules"] = std::move(rows);
        return json.dump(2) + "\n";
    }

    void clear_ecocov_files(const std::filesystem::path &directory)
    {
        std::error_code ec;
        if (!std::filesystem::exists(directory, ec)) {
            return;
        }

        for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".ecocov") {
                Compiler::clear_artifact_path(entry.path());
            }
        }
    }

    bool prepare_coverage_dirs(const Compiler::BuildLayout &layout, std::string &out_error)
    {
        std::error_code ec;
        const std::filesystem::path tests = tests_dir(layout);
        std::filesystem::create_directories(tests, ec);
        if (ec) {
            out_error = fmt::format("could not create '{}': {}", tests.string(), ec.message());
            return false;
        }

        clear_ecocov_files(tests);
        return true;
    }

    struct ModuleCounts
    {
        std::string name;
        std::vector<uint32_t> counts;
    };

    std::optional<std::vector<ModuleCounts>> read_counts(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }

        eco_cov_header header {};
        in.read(reinterpret_cast<char *>(&header), sizeof(header));
        if (!in || std::memcmp(header.magic, ECO_COV_MAGIC, 6) != 0
            || header.version != ECO_COV_VERSION) {
            return std::nullopt;
        }

        std::vector<ModuleCounts> modules;
        modules.reserve(header.n_modules);

        for (uint32_t i = 0; i < header.n_modules; i++) {
            uint32_t name_len = 0;
            uint32_t n = 0;
            in.read(reinterpret_cast<char *>(&name_len), sizeof(name_len));
            in.read(reinterpret_cast<char *>(&n), sizeof(n));
            if (!in) {
                return std::nullopt;
            }

            const uint32_t name_bytes = (name_len + 3u) & ~3u;
            std::string name(name_bytes, '\0');
            if (name_bytes > 0) {
                in.read(name.data(), static_cast<std::streamsize>(name_bytes));
                if (!in) {
                    return std::nullopt;
                }
            }
            name.resize(name_len);

            ModuleCounts row;
            row.name = std::move(name);
            row.counts.assign(n, 0);
            if (n > 0) {
                in.read(
                    reinterpret_cast<char *>(row.counts.data()),
                    static_cast<std::streamsize>(n * sizeof(uint32_t)));
                if (!in) {
                    return std::nullopt;
                }
            }
            modules.push_back(std::move(row));
        }

        return modules;
    }

    std::string module_json_text(
        const std::string &name,
        const std::vector<Compiler::CoverageSite> &sites
    )
    {
        nlohmann::ordered_json module;
        module["name"] = name;
        if (!sites.empty() && !sites.front().dir.empty()) {
            module["dir"] = sites.front().dir;
        }
        module["counters"] = counters_json(sites);
        return module.dump(2) + "\n";
    }

    bool write_module_json(
        const std::filesystem::path &dest,
        const std::string &name,
        const std::vector<Compiler::CoverageSite> &sites,
        std::string &out_error
    )
    {
        if (!Compiler::write_artifact_file(dest, module_json_text(name, sites))) {
            out_error = fmt::format("could not write '{}'", dest.string());
            return false;
        }

        return true;
    }

    std::optional<std::vector<Compiler::CoverageSite>> load_module_json(
        const std::filesystem::path &path
    )
    {
        const std::optional<std::string> json = Compiler::read_whole_file(path);
        if (!json.has_value()) {
            return std::nullopt;
        }

        nlohmann::json parsed = nlohmann::json::parse(*json, nullptr, false);
        if (parsed.is_discarded()) {
            return std::nullopt;
        }

        return sites_from_json(parsed);
    }
};

bool Compiler::coverage_map_present(const std::filesystem::path &object)
{
    return load_module_json(coverage_object_map_path(object)).has_value();
}

bool Compiler::coverage_allows_reuse(
    const CompilerOptions &options,
    bool compiler_supplied,
    const std::filesystem::path &object
)
{
    if (!options.coverage_instruments_module(compiler_supplied)) {
        return true;
    }

    return coverage_map_present(object);
}

bool Compiler::write_coverage_map(
    const BuildLayout &layout,
    const CoverageModuleSites &compiled,
    const std::set<std::string> &cached_modules,
    const std::map<std::string, std::filesystem::path> &objects,
    std::string &out_error
)
{
    if (!prepare_coverage_dirs(layout, out_error)) {
        return false;
    }

    CoverageModuleSites merged;

    for (const auto &[name, sites] : compiled) {
        // **ODR copies drained into another unit still record sites under this name.**
        // a cache hit has no CmpUnit, so those few inlines would be the whole map
        if (cached_modules.count(name) > 0) {
            continue;
        }

        merged.emplace(name, sites);

        auto stored = objects.find(name);
        if (stored == objects.end()) {
            continue;
        }

        if (!write_module_json(coverage_object_map_path(stored->second), name, sites, out_error)) {
            return false;
        }
    }

    for (const std::string &name : cached_modules) {
        // uninstrumented hits (stdlib without `--coverage-stdlib`) have no sidecar
        auto stored = objects.find(name);
        if (stored == objects.end()) {
            continue;
        }

        const std::filesystem::path sidecar = coverage_object_map_path(stored->second);
        const std::optional<std::vector<CoverageSite>> sites = load_module_json(sidecar);
        if (!sites.has_value()) {
            out_error = fmt::format("could not read coverage map '{}'", sidecar.string());
            return false;
        }

        merged.emplace(name, *sites);
    }

    if (!write_artifact_file(coverage_map_path(layout), map_json_of(merged))) {
        out_error = fmt::format("could not write '{}'", coverage_map_path(layout).string());
        return false;
    }

    return true;
}

void Compiler::coverage_prepare_test(
    const BuildLayout &layout,
    const TestCase &test,
    std::vector<std::pair<std::string, std::string>> &extra_env
)
{
    // ECHO_TEST_ID, which the runtime stamps into the header, comes from run_test_isolated
    extra_env.emplace_back("ECHO_COVERAGE_OUT", coverage_test_path(layout, test).string());
}

void Compiler::coverage_report_run(const BuildLayout &layout, std::ostream &out)
{
    const std::optional<std::string> json = read_whole_file(coverage_map_path(layout));
    if (!json.has_value()) {
        return;
    }

    nlohmann::json map = nlohmann::json::parse(*json, nullptr, false);
    if (map.is_discarded() || !map.contains("modules") || !map["modules"].is_array()) {
        return;
    }

    std::map<std::string, std::vector<uint32_t>> merged;
    for (const nlohmann::json &module : map["modules"]) {
        const std::string name = module.value("name", "");
        const size_t n = module.contains("counters") && module["counters"].is_array()
            ? module["counters"].size()
            : 0;
        merged[name].assign(n, 0);
    }

    std::error_code ec;
    const std::filesystem::path dir = tests_dir(layout);
    if (std::filesystem::exists(dir, ec)) {
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".ecocov") {
                continue;
            }

            const std::optional<std::vector<ModuleCounts>> counts = read_counts(entry.path());
            if (!counts.has_value()) {
                continue;
            }

            for (const ModuleCounts &row : *counts) {
                auto found = merged.find(row.name);
                if (found == merged.end() || found->second.size() != row.counts.size()) {
                    continue;
                }

                for (size_t i = 0; i < row.counts.size(); i++) {
                    const uint32_t a = found->second[i];
                    const uint32_t b = row.counts[i];
                    found->second[i] = a > 0xffffffffu - b ? 0xffffffffu : a + b;
                }
            }
        }
    }

    // **one source line is one line, however many bodies were compiled from it.** a generic is
    // instrumented once per instance, so counting counters would score `List<T>::show` as missed
    // because its `List<string>` copy never ran although its `List<int32>` one did. a line, and a
    // branch edge at its column, is covered when any counter planted there ran
    std::map<std::string, bool> line_seen;
    std::map<std::string, bool> branch_seen;

    for (const nlohmann::json &module : map["modules"]) {
        const std::string name = module.value("name", "");
        auto found = merged.find(name);
        if (found == merged.end() || !module.contains("counters")) {
            continue;
        }

        const nlohmann::json &counters = module["counters"];
        for (size_t i = 0; i < counters.size() && i < found->second.size(); i++) {
            const nlohmann::json &counter = counters[i];
            const std::string kind = counter.value("kind", "");
            const bool hit = found->second[i] > 0;
            const std::string at = fmt::format(
                "{}\n{}\n{}", name, counter.value("file", ""), counter.value("line", 0));

            if (kind == "line") {
                bool &seen = line_seen[at];
                seen = seen || hit;
            }
            else if (kind == "branch") {
                bool &seen = branch_seen[fmt::format(
                    "{}\n{}\n{}", at, counter.value("column", 0), counter.value("edge", ""))];
                seen = seen || hit;
            }
        }
    }

    size_t lines = line_seen.size();
    size_t lines_hit = 0;
    size_t branches = branch_seen.size();
    size_t branches_hit = 0;

    for (const auto &[at, hit] : line_seen) {
        lines_hit += hit ? 1 : 0;
    }

    for (const auto &[at, hit] : branch_seen) {
        branches_hit += hit ? 1 : 0;
    }

    const double line_pct = lines == 0
        ? 100.0
        : (100.0 * static_cast<double>(lines_hit) / static_cast<double>(lines));
    const double branch_pct = branches == 0
        ? 100.0
        : (100.0 * static_cast<double>(branches_hit) / static_cast<double>(branches));

    out << fmt::format(
        "coverage {:.1f}% lines, {:.1f}% branches\n",
        line_pct,
        branch_pct);
}
