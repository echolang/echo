#include "Compiler/BuildHooks.h"

#include "Compiler/BuildLayout.h"
#include "Compiler/CommandLineOption.h"
#include "Compiler/HostTool.h"
#include "Compiler/ParsePipeline.h"
#include "Compiler/ProgressReporter.h"
#include "Compiler/PublicTools.h"

#include <fmt/core.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{

// a small stated protocol, not a dump of the build: module, state dir, target, this echoc
std::vector<std::pair<std::string, std::string>> hook_environment(
    const Compiler::DriverOptions &driver,
    const Compiler::TargetFacts &facts,
    const Parser::ModuleManifest &root,
    const std::filesystem::path &state
)
{
    return {
        { "ECHO_MODULE_DIR", root.directory.string() },
        { "ECHO_HOOK_STATE", state.string() },
        { "ECHO_TARGET_OS", facts.operating_system },
        { "ECHO_TARGET_ARCH", facts.architecture },
        { "ECHO_MODE", driver.options.assertions_enabled() ? "debug" : "release" },
        { "ECHO_COMMAND", Compiler::subcommand_info(driver.subcommand).name },
        { "ECHOC", Compiler::process_executable().string() },
    };
}

};

// one root's hooks, in declaration order, from that root's own directory. `package_dir` is the
// invocation's, which several roots share. `built` is every tool build this invocation has already
// run, as the command that ran it: two roots hooking the same tool build it once between them, and
// the command is the whole of what a build is a function of
static bool run_root_hooks(
    const Compiler::DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    const Compiler::TargetFacts &facts,
    const Parser::ModuleManifest *loaded,
    const std::filesystem::path &package_dir,
    std::set<std::vector<std::string>> &built
)
{
    using namespace Compiler;

    if (loaded == nullptr || loaded->hooks.empty()) {
        return true;
    }

    ToolConsumer consumer(facts);
    settle_package_dir({ loaded->path }, package_dir, consumer.root_scratch);
    consumer.host_scratch.package_dir = consumer.root_scratch.package_dir;
    consumer.host_scratch.entry_directory = consumer.root_scratch.entry_directory;
    consumer.root = *loaded;

    const Parser::ModuleManifest &root = consumer.root;

    const auto refused = [&]() {
        consumer.print_issues(diagnostics);
        return false;
    };

    PublicToolSet tools;

    if (!consumer.resolve_tools(driver.build_dir, tools)) {
        return refused();
    }

    const BuildLayout layout = BuildLayout::resolve(driver.build_dir, &root, { root.path }, {});
    const bool json = diagnostics.is_machine_readable();

    for (const Parser::ModuleHook &hook : root.hooks) {
        // deferred from the read: refused now that something needs them
        if (!Parser::refuse_unresolved_requirements(
                root, hook.unresolved, fmt::format("hook '{}'", hook.name), consumer.root_scratch)) {
            return refused();
        }

        std::string refusal;
        const PublicTool *tool = select_public_tool(tools, hook.run, refusal);

        if (tool == nullptr) {
            consumer.root_scratch.report_at_line<AST::Issue::UnresolvableHookTool>(
                root.path, hook.line, fmt::format("hook '{}' cannot run: {}", hook.name, refusal)
            );
            return refused();
        }

        // two hooks sharing a tool build it once, before the child would have to load itself to notice
        if (built.insert(tool->build_argv).second) {
            ProgressStep step(ProgressReporter::instance(), ProgressPhase::t_tool, tool->refs.front());

            const CapturedProcess build = run_captured(tool->build_argv);
            step.finish(build.exit_code == 0);

            if (build.exit_code != 0) {
                consumer.root_scratch.report_at_line<AST::Issue::HookToolBuildFailed>(
                    root.path, hook.line, fmt::format(
                        "hook '{}' cannot run: its tool '{}:{}' did not build.",
                        hook.name, tool->module, tool->target
                    ),
                    build.output
                );
                return refused();
            }
        }

        // a `--build-dir` somebody else owns, or a directory that cannot be created, is refused here
        const BuildDirState prepared = layout.prepare_hook_state_dir(root, hook.name);

        if (prepared != BuildDirState::t_ready) {
            const std::string state = layout.hook_state_dir(root, hook.name).string();

            if (prepared == BuildDirState::t_foreign) {
                diagnostics.render_untyped("Build Directory In Use", fmt::format(
                    "'{}' already holds something echoc did not put there, so hook '{}' has nowhere to keep "
                    "its state.",
                    layout.module_dir(root).string(), hook.name
                ));
            }
            else {
                diagnostics.render_untyped("Cannot Build Here", fmt::format(
                    "'{}' could not be created, so hook '{}' has nowhere to keep its state.",
                    state, hook.name
                ));
            }

            return false;
        }

        std::vector<std::string> argv = { tool->binary.string() };
        argv.insert(argv.end(), hook.args.begin(), hook.args.end());

        const std::vector<std::pair<std::string, std::string>> environment =
            hook_environment(driver, facts, root, layout.hook_state_dir(root, hook.name));

        ProgressStep step(ProgressReporter::instance(), ProgressPhase::t_hook, hook.name);

        // streamed onto stderr so the program's stdout stays the program's. json captures it as a note
        int status = 0;
        std::string output;

        if (json) {
            const CapturedProcess ran = run_captured(argv, 0, root.directory, environment);
            status = ran.exit_code;
            output = ran.output;
        }
        else {
            status = run_streamed(argv, root.directory, environment);
        }

        step.finish(status == 0);

        if (status != 0) {
            consumer.root_scratch.report_at_line<AST::Issue::HookFailed>(
                root.path, hook.line, fmt::format(
                    "hook '{}' failed: '{}' exited with {}.",
                    hook.name, tool->refs.front(), status
                ),
                output
            );
            return refused();
        }
    }

    return true;
}

bool Compiler::run_before_build_hooks(
    const DriverOptions &driver,
    const AST::DiagnosticRenderer &diagnostics,
    const TargetFacts &facts,
    const std::vector<Parser::ModuleManifest> &manifests,
    const std::vector<std::filesystem::path> &user_roots
)
{
    if (driver.no_hooks || !driver.sources.empty()) {
        return true;
    }

    // **a test run over several roots builds every one of them, so every one's hooks run.** each from
    // its own directory and into its own state, exactly as a run pointed at that root alone would. any
    // other command with several roots has one program among them at most, and running a hook for a
    // module that is only a library to it is what "only the root runs its hooks" refuses
    if (user_roots.size() != 1 && driver.subcommand != Subcommand::t_test) {
        for (const std::filesystem::path &root : user_roots) {
            const Parser::ModuleManifest *manifest = manifest_at_root(manifests, root);

            if (manifest == nullptr || manifest->hooks.empty()) {
                continue;
            }

            diagnostics.render_untyped(
                "Several Roots",
                "a '#[hook:]' runs for one module - name one '-m', or drop the extra roots.");
            return false;
        }

        return true;
    }

    // **one package directory for the run, the first root's** - the rule the build itself follows,
    // so a hook's tool is the package the program is compiled against and not a second copy in
    // another root's `vendor/`
    std::filesystem::path package_dir = driver.package_dir;

    if (package_dir.empty() && user_roots.size() > 1) {
        package_dir = package_dir_for(user_roots.front(), {}, facts);
    }

    std::set<std::vector<std::string>> built;

    for (const std::filesystem::path &root : user_roots) {
        if (!run_root_hooks(driver, diagnostics, facts, manifest_at_root(manifests, root), package_dir, built)) {
            return false;
        }
    }

    return true;
}
