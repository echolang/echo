#include "Compiler/PublicTools.h"

#include "Compiler/BuildLayout.h"
#include "Compiler/CodegenTarget.h"
#include "Compiler/HostTool.h"
#include "Compiler/ParsePipeline.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace
{

std::string qualified_ref(const std::string &module, const std::string &target)
{
    return module + ":" + target;
}

// shortest name that still selects each tool: bare when unique, else qualified
std::vector<std::string> offered_refs(const Compiler::PublicToolSet &set)
{
    std::vector<std::string> refs;

    for (const Compiler::PublicTool &tool : set.tools) {
        refs.push_back(tool.refs.front());
    }

    return refs;
}

// child build of one tool. `--no-hooks`: the dependency's hooks are its author's. no `--target-os`:
// the tool runs here
std::vector<std::string> tool_build_argv(
    const std::filesystem::path &manifest,
    const std::string &target,
    const std::filesystem::path &package_dir,
    const std::filesystem::path &build_dir_flag
)
{
    std::vector<std::string> argv = {
        Compiler::process_executable().string(),
        "build",
        "-m", manifest.string(),
        "--target", target,
        "--package-dir", package_dir.string(),
        "--no-hooks",
    };

    if (!build_dir_flag.empty()) {
        argv.push_back("--build-dir");
        argv.push_back(build_dir_flag.string());
    }

    return argv;
}

std::string nothing_else_offered(const Compiler::PublicToolSet &set)
{
    if (set.tools.empty()) {
        return fmt::format(
            "No module '{}' depends on offers a tool - a dependency offers one with "
            "'#[target: exe {{ ..., public: true }}]'.",
            set.consumer);
    }

    return fmt::format("The tools on offer are: {}.", fmt::join(offered_refs(set), ", "));
}

};

Compiler::ToolConsumer::ToolConsumer(const TargetFacts &facts) :
    root_scratch(facts),
    host_scratch(TargetFacts::host())
{
}

bool Compiler::ToolConsumer::read(
    const std::filesystem::path &manifest,
    const std::filesystem::path &package_dir_override
)
{
    // one settlement, copied: host and program resolve `#[requires:]` against the same package dir
    settle_package_dir({ manifest }, package_dir_override, root_scratch);
    host_scratch.package_dir = root_scratch.package_dir;
    host_scratch.entry_directory = root_scratch.entry_directory;

    return Parser::read_module_manifest(manifest, root_scratch, root, Parser::ManifestRead::t_dependencies);
}

void Compiler::ToolConsumer::print_issues(const AST::DiagnosticRenderer &diagnostics)
{
    root_scratch.bundle.collector.print_issues(diagnostics);
    host_scratch.bundle.collector.print_issues(diagnostics);
}

bool Compiler::ToolConsumer::resolve_tools(const std::filesystem::path &build_dir_flag, PublicToolSet &out)
{
    out = PublicToolSet{};
    out.consumer = root.name;

    for (const Parser::ModuleTarget &target : root.targets) {
        if (target.kind == Parser::TargetKind::t_executable) {
            out.own_targets.push_back(qualified_ref(root.name, target.name));
        }
    }

    std::vector<std::filesystem::path> direct = root.depends;
    Parser::append_hook_depends(root, direct);

    // host row: a tool runs here, so `--target-os wasi` must not name a `.wasm` to execute
    const Compiler::CodegenTarget host_row;

    for (const std::filesystem::path &path : direct) {
        Parser::ModuleManifest dependency;

        // as written is enough: name, kind, `public`. a missing entry is the tool's own build to refuse
        if (!Parser::read_module_manifest(path, host_scratch, dependency, Parser::ManifestRead::t_written)) {
            return false;
        }

        out.dependencies.push_back(dependency.name);

        const Compiler::BuildLayout layout =
            Compiler::BuildLayout::resolve(build_dir_flag, &dependency, {});

        for (const Parser::ModuleTarget &target : dependency.targets) {
            if (target.kind != Parser::TargetKind::t_executable) {
                continue;
            }

            if (!target.is_public) {
                out.unoffered.push_back(qualified_ref(dependency.name, target.name));
                continue;
            }

            PublicTool tool;
            tool.module = dependency.name;
            tool.target = target.name;
            tool.manifest = dependency.path;
            tool.binary = host_row.output_path(layout.target_binary(dependency, target.name));
            tool.build_argv =
                tool_build_argv(dependency.path, target.name, host_scratch.package_dir, build_dir_flag);

            out.tools.push_back(std::move(tool));
        }
    }

    // a bare name selects only when unique: two `gen`s is a refusal that lists both
    std::map<std::string, size_t> bare_count;

    for (const PublicTool &tool : out.tools) {
        bare_count[tool.target]++;
    }

    for (PublicTool &tool : out.tools) {
        if (bare_count[tool.target] == 1) {
            tool.refs.push_back(tool.target);
        }

        tool.refs.push_back(qualified_ref(tool.module, tool.target));
    }

    return true;
}

const Compiler::PublicTool *Compiler::select_public_tool(
    const PublicToolSet &set,
    const std::string &ref,
    std::string &out_refusal
)
{
    for (const PublicTool &tool : set.tools) {
        if (std::find(tool.refs.begin(), tool.refs.end(), ref) != tool.refs.end()) {
            return &tool;
        }
    }

    const size_t colon = ref.find(':');
    const bool qualified = colon != std::string::npos;
    const std::string module = qualified ? ref.substr(0, colon) : std::string();
    const std::string target = qualified ? ref.substr(colon + 1) : ref;

    const auto names = [&target](const std::string &candidate) {
        return candidate.substr(candidate.find(':') + 1) == target;
    };

    // own program: generating into the module you are building is a cycle. `echoc run --target` instead
    for (const std::string &own : set.own_targets) {
        if ((qualified && own == ref) || (!qualified && names(own))) {
            out_refusal = fmt::format(
                "'{}' is one of '{}''s own targets, and a module runs only the tools its dependencies offer. "
                "Move it into a module of its own and depend on that, or run it with "
                "'echoc run --target {}'.",
                ref, set.consumer, target);
            return nullptr;
        }
    }

    if (!qualified) {
        std::vector<std::string> sharing;

        for (const PublicTool &tool : set.tools) {
            if (tool.target == target) {
                sharing.push_back(qualified_ref(tool.module, tool.target));
            }
        }

        if (sharing.size() > 1) {
            out_refusal = fmt::format(
                "'{}' is offered by more than one dependency - name the one you mean: {}.",
                ref, fmt::join(sharing, ", "));
            return nullptr;
        }
    }

    for (const std::string &held : set.unoffered) {
        if ((qualified && held == ref) || (!qualified && names(held))) {
            out_refusal = fmt::format(
                "'{}' is a target of a dependency but is not offered to the modules that depend on it - its "
                "manifest would have to say 'public: true'. {}",
                held, nothing_else_offered(set));
            return nullptr;
        }
    }

    if (qualified
        && std::find(set.dependencies.begin(), set.dependencies.end(), module) == set.dependencies.end()) {
        out_refusal = fmt::format(
            "'{}' is not a module '{}' depends on directly, so it has nothing to offer here. {}",
            module, set.consumer, nothing_else_offered(set));
        return nullptr;
    }

    out_refusal = fmt::format(
        "no dependency of '{}' offers a tool called '{}'. {}",
        set.consumer, ref, nothing_else_offered(set)
    );
    return nullptr;
}

std::string Compiler::public_tools_as_json(const PublicToolSet &set, const std::filesystem::path &package_dir)
{
    std::ostringstream out;

    out << "{\n  \"module\": " << Parser::json_quoted(set.consumer);
    out << ",\n  \"package_dir\": " << Parser::json_quoted(package_dir.string());
    out << ",\n  \"public_tools\": [";

    for (size_t i = 0; i < set.tools.size(); i++) {
        const PublicTool &tool = set.tools[i];

        out << (i == 0 ? "\n" : ",\n");
        out << "    { \"refs\": " << Parser::json_quoted_list(tool.refs);
        out << ", \"module\": " << Parser::json_quoted(tool.module);
        out << ", \"target\": " << Parser::json_quoted(tool.target);
        out << ", \"manifest\": " << Parser::json_quoted(tool.manifest.string());
        out << ", \"binary\": " << Parser::json_quoted(tool.binary.string());
        out << ", \"build\": " << Parser::json_quoted_list(tool.build_argv);
        out << " }";
    }

    out << (set.tools.empty() ? "]" : "\n  ]") << "\n}\n";

    return out.str();
}
