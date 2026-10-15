#include "Compiler/ProgramStamp.h"

#include "eco.h"
#include "Compiler/CBuild.h"
#include "Compiler/CodegenTarget.h"
#include "Compiler/Fnv.h"
#include "Compiler/HostTool.h"
#include "Compiler/ModuleCache.h"
#include "Compiler/ParsePipeline.h"

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{

uint64_t fold_flag(bool on, const char *yes, const char *no, uint64_t digest)
{
    return Compiler::fnv1a64(std::string(on ? yes : no), digest);
}

// a field and its terminator, so `ab`+`c` and `a`+`bc` cannot fold alike
uint64_t fold_field(const std::string &text, uint64_t digest)
{
    digest = Compiler::fnv1a64(text, digest);
    return Compiler::fnv1a64("\x1f", 1, digest);
}

// size and mtime of the binary a build wrote, without reading it
struct OutputIdentity
{
    std::uintmax_t size = 0;
    long long mtime = 0;
};

std::optional<OutputIdentity> identity_of(const std::filesystem::path &output)
{
    std::error_code ec;

    if (!std::filesystem::is_regular_file(output, ec)) {
        return std::nullopt;
    }

    OutputIdentity identity;
    identity.size = std::filesystem::file_size(output, ec);

    if (ec) {
        return std::nullopt;
    }

    const std::filesystem::file_time_type written = std::filesystem::last_write_time(output, ec);

    if (ec) {
        return std::nullopt;
    }

    identity.mtime = std::chrono::duration_cast<std::chrono::nanoseconds>(written.time_since_epoch()).count();

    return identity;
}

// Darwin `-g` leaves a `.dSYM` when dsymutil ran. record whether there was one: requiring it
// forever-rebuilds on a machine without dsymutil
bool has_dsym(const std::filesystem::path &output)
{
    std::filesystem::path dsym = output;
    dsym += ".dSYM";

    std::error_code ec;
    return std::filesystem::is_directory(dsym, ec);
}

// the Echo key's own line, so a record can be refused on it before the C half is probed
std::string echo_line(uint64_t echo_key)
{
    return fmt::format("\necho {}\n", Compiler::to_hex(echo_key));
}

std::string stamp_contents(uint64_t echo_key, uint64_t c_key, const OutputIdentity &identity, bool dsym)
{
    return fmt::format(
        "# echoc program stamp: what the binary beside it was built from. Deleting it rebuilds.{}"
        "c {}\nsize {}\nmtime {}\ndsym {}\n",
        echo_line(echo_key), Compiler::to_hex(c_key), identity.size, identity.mtime, dsym ? "yes" : "no");
}

bool compute_program_echo_key(
    const Compiler::ProgramStampPlan &plan,
    uint64_t &out_key,
    std::map<std::string, Compiler::ModuleCacheKey> &out_module_keys,
    std::string &out_error
)
{
    std::map<std::string, Compiler::ModuleCacheKey> &keys = out_module_keys;

    if (!Compiler::compute_module_keys(
            plan.manifests, plan.options, plan.options.facts, plan.test_modules, plan.active_targets,
            plan.optimize_whole, keys, out_error)) {
        return false;
    }

    uint64_t digest = Compiler::k_fnv_offset_basis;

    digest = fold_field(ECO_VERSION_STRING, digest);

    for (const Parser::ModuleManifest *manifest : plan.manifests) {
        const auto found = keys.find(manifest->name);

        if (found == keys.end()) {
            out_error = fmt::format("internal: '{}' has no module key.", manifest->name);
            return false;
        }

        digest = fold_field(manifest->name, digest);
        digest = fold_field(found->second.hex, digest);
    }

    digest = fold_flag(plan.options.report_allocations, "report", "noreport", digest);

    digest = fold_field(plan.program_name, digest);
    digest = fold_field(plan.entry_file.string(), digest);
    digest = fold_field(plan.output.string(), digest);

    for (const Compiler::LinkRequirement &requirement : plan.link) {
        digest = fold_field(Compiler::link_requirement_spelling(requirement), digest);
        digest = fold_flag(requirement.linkage == Compiler::LinkLinkage::t_static, "static", "dynamic", digest);
        digest = fold_field(requirement.file.value_or(""), digest);

        if (requirement.scheme == Compiler::LinkScheme::t_object) {
            const std::optional<std::string> bytes = Compiler::read_whole_file(requirement.value);

            if (!bytes.has_value()) {
                out_error = fmt::format("{}: cannot be read to key the program.", requirement.value);
                return false;
            }

            digest = Compiler::fnv1a64(bytes.value(), digest);
        }
    }

    digest = fold_field(Compiler::cc_driver(plan.options.codegen), digest);
    digest = Compiler::fold_toolchain_sysroots(plan.options, digest);

    out_key = digest;
    return true;
}

bool compute_program_c_key(
    const Compiler::ProgramStampPlan &plan,
    const Compiler::BuildLayout &layout,
    uint64_t &out_key,
    std::string &out_error
)
{
    uint64_t digest = Compiler::k_fnv_offset_basis;

    for (const Parser::ModuleManifest *manifest : plan.manifests) {
        const Parser::ModuleContribution contribution =
            Parser::module_contribution_for(*manifest, plan.active_targets);

        if (contribution.cc.empty()) {
            continue;
        }

        uint64_t module_digest = 0;

        if (!Compiler::probe_c_content_digest(
                contribution.cc, plan.options, layout.module_cc_dir(*manifest), layout.scratch_cc_dir(*manifest),
                module_digest, out_error)) {
            return false;
        }

        digest = Compiler::fnv1a64(&module_digest, sizeof(module_digest), digest);
    }

    out_key = digest;
    return true;
}

bool program_is_current(
    const std::filesystem::path &stamp,
    uint64_t echo_key,
    const std::function<std::optional<uint64_t>()> &c_key,
    const std::filesystem::path &output
)
{
    const std::optional<std::string> recorded = Compiler::read_whole_file(stamp);

    if (!recorded.has_value() || recorded->find(echo_line(echo_key)) == std::string::npos) {
        return false;
    }

    const std::optional<OutputIdentity> identity = identity_of(output);

    if (!identity.has_value()) {
        return false;
    }

    const std::optional<uint64_t> probed = c_key();

    if (!probed.has_value()) {
        return false;
    }

    return recorded.value() == stamp_contents(echo_key, probed.value(), identity.value(), has_dsym(output));
}

void write_program_stamp(
    const std::filesystem::path &stamp,
    uint64_t echo_key,
    uint64_t c_key,
    const std::filesystem::path &output
)
{
    const std::optional<OutputIdentity> identity = identity_of(output);

    if (!identity.has_value()) {
        return;
    }

    std::ofstream(stamp, std::ios::binary | std::ios::trunc)
        << stamp_contents(echo_key, c_key, identity.value(), has_dsym(output));
}

std::optional<uint64_t> program_stamp_c_key(
    const Compiler::ProgramStampPlan &plan, const Compiler::BuildLayout &layout)
{
    uint64_t c_key = 0;
    std::string error;

    if (!compute_program_c_key(plan, layout, c_key, error)) {
        return std::nullopt;
    }

    return c_key;
}

};

bool Compiler::collect_program_link(
    const std::vector<const Parser::ModuleManifest *> &manifests,
    const Parser::ActiveTargets &active_targets,
    const std::vector<std::string> &command_line,
    const TargetFacts &facts,
    std::vector<LinkRequirement> &out,
    std::string &out_error
)
{
    for (auto manifest = manifests.rbegin(); manifest != manifests.rend(); ++manifest) {
        std::string error;
        const Parser::ModuleContribution contribution =
            Parser::module_contribution_for(**manifest, active_targets, &error);

        if (!error.empty()) {
            out_error = std::move(error);
            return false;
        }

        if (!merge_link_requirements(contribution.link, out, out_error)) {
            return false;
        }
    }

    if (command_line.empty()) {
        return true;
    }

    // resolved against the working directory, unlike a manifest's: a command line legitimately means
    // "here", where a manifest has to mean the same thing wherever echoc was run from
    const std::filesystem::path here = std::filesystem::current_path();

    std::vector<LinkRequirement> from_command_line;

    for (const std::string &spelled : command_line) {
        LinkRequirement requirement;

        if (!parse_link_requirement(spelled, here, facts, /*declared_by=*/"", requirement, out_error)) {
            return false;
        }

        from_command_line.push_back(requirement);
    }

    return merge_link_requirements(from_command_line, out, out_error);
}

bool Compiler::plan_program_stamp(
    const ProgramStampRequest &request,
    std::optional<ProgramStampPlan> &out,
    std::string &out_error
)
{
    out.reset();
    out_error.clear();

    if (!request.can_stamp()) {
        return true;
    }

    const auto entry = std::find_if(
        request.all_manifests.begin(), request.all_manifests.end(),
        [&request](const Parser::ModuleManifest &manifest) { return manifest.name == request.entry_module; });

    if (entry == request.all_manifests.end()) {
        return true;
    }

    ProgramStampPlan plan;
    plan.path = request.layout.program_stamp(*entry, request.program_name, request.output);
    plan.manifests = program_manifests(request.all_manifests, request.active_targets);
    plan.options = request.options;
    plan.active_targets = request.active_targets;
    plan.test_modules = request.test_modules;
    plan.optimize_whole = request.optimize_whole;
    plan.program_name = request.program_name;
    plan.entry_file = request.entry_file;
    plan.output = request.output;

    if (!collect_program_link(
            plan.manifests, request.active_targets, request.command_line_link, request.facts, plan.link,
            out_error)) {
        return false;
    }

    if (!compute_program_echo_key(plan, plan.echo_key, plan.module_keys, out_error)) {
        // an unreadable source is a build that is about to say so; no stamp rather than a second refusal
        out_error.clear();
        return true;
    }

    out = std::move(plan);
    return true;
}

bool Compiler::program_stamp_is_current(const ProgramStampPlan &plan, const BuildLayout &layout)
{
    const auto c_key = [&]() { return program_stamp_c_key(plan, layout); };

    return program_is_current(plan.path, plan.echo_key, c_key, plan.output);
}

void Compiler::write_built_program_stamp(const ProgramStampPlan &plan, const BuildLayout &layout)
{
    if (const std::optional<uint64_t> c_key = program_stamp_c_key(plan, layout)) {
        write_program_stamp(plan.path, plan.echo_key, c_key.value(), plan.output);
    }
}
