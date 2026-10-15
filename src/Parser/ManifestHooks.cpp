#include "Parser/ManifestHooks.h"

#include "Compiler/LinkRequirement.h"

#include "AST/ASTIssue.h"

#include <fmt/core.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

// the moments a hook may run at. **one row per spelling**, read through AttributeReader::tag exactly as a
// target's kind is, so a misspelled moment is refused in the words a misspelled `#[link:]` scheme is
const std::vector<std::pair<std::string, Parser::HookMoment>> &hook_moment_table()
{
    static const std::vector<std::pair<std::string, Parser::HookMoment>> table = {
        { "before_build", Parser::HookMoment::t_before_build },
    };

    return table;
}

};

void Parser::read_manifest_hooks(
    const AST::AttributeValue &written,
    AST::AttributeReader &reader,
    uint32_t line,
    std::vector<Parser::WrittenHook> &out_hooks
)
{
    for (const AST::AttributeValue *entry : AST::AttributeReader::each(written)) {
        if (!entry->has_tag()) {
            reader.refuse(entry->span, fmt::format(
                "a hook says when it runs - write '#[hook: before_build {{ run: \"...\" }}]'. "
                "The moments are: {}.",
                Compiler::scheme_list_of(hook_moment_table())));
            continue;
        }

        Parser::HookMoment moment = Parser::HookMoment::t_before_build;

        if (!reader.tag(*entry, hook_moment_table(), "hook moment", Compiler::scheme_list_of(hook_moment_table()), moment)) {
            continue;
        }

        for (const AST::AttributeValue *record : AST::AttributeReader::payload(*entry)) {
            if (!reader.record(*record)) {
                continue;
            }

            const bool named_its_tool = reader.require_field(*record, "run") != nullptr;
            reader.reject_unknown_fields(*record, { "run", "args", "name" });

            if (!named_its_tool) {
                continue;
            }

            Parser::WrittenHook hook;
            hook.moment = moment;
            hook.line = record->span.is_valid() ? record->span.line() : line;

            std::optional<std::string> run = reader.string(*reader.field(*record, "run"));

            if (!run.has_value()) {
                continue;
            }

            hook.run = run.value();

            if (const AST::AttributeValue *name = reader.field(*record, "name")) {
                std::optional<std::string> spelled = reader.string(*name);

                if (!spelled.has_value()) {
                    continue;
                }

                hook.name = spelled.value();
            }

            // through AttributeReader::each, so one argument and a list of them are the same thing written
            // two ways - the rule every list-valued field in a manifest follows
            if (const AST::AttributeValue *args = reader.field(*record, "args")) {
                for (const AST::AttributeValue *arg : AST::AttributeReader::each(*args)) {
                    if (std::optional<std::string> spelled = reader.string(*arg)) {
                        hook.args.push_back(spelled.value());
                    }
                }
            }

            out_hooks.push_back(std::move(hook));
        }
    }
}

bool Parser::settle_manifest_hooks(
    const std::vector<Parser::WrittenHook> &written,
    Parser::ModuleManifest &out,
    bool resolve,
    const std::filesystem::path &package_dir,
    const std::filesystem::path &entry_directory,
    const Parser::ManifestReport &into
)
{
    for (const Parser::WrittenHook &hook : written) {
        Parser::ModuleHook settled;
        settled.moment = hook.moment;
        settled.run = hook.run;
        settled.args = hook.args;
        settled.line = hook.line;
        settled.depends_as_written = hook.scope.scoped_depends;
        settled.requirements = hook.scope.scoped_requirements;

        // the tool's target name, which is the half of `<module>:<target>` a person reads as "the tool"
        settled.name = hook.name.empty() ? hook.run.substr(hook.run.find(':') + 1) : hook.name;

        if (!Parser::is_usable_artifact_name(settled.name)) {
            Parser::report_at<AST::Issue::UnusableTargetName>(into, hook.line, fmt::format(
                "'{}' is not a usable hook name - it names the hook's directory in the build directory, so it "
                "may hold only letters, digits, '_' and '-'. Give the hook a 'name:'.",
                settled.name
            ));
            return false;
        }

        for (const Parser::ModuleHook &earlier : out.hooks) {
            if (earlier.name != settled.name) {
                continue;
            }

            Parser::report_at<AST::Issue::RepeatedManifestAttribute>(into, hook.line, fmt::format(
                "two hooks are called '{}' - a hook's name is its directory, so give one of them a "
                "different 'name:'.",
                settled.name
            ));
            return false;
        }

        if (resolve
            && !Parser::resolve_scope_edges(
                hook.scope, out, package_dir, entry_directory, settled.depends, settled.unresolved, into)) {
            return false;
        }

        out.hooks.push_back(std::move(settled));
    }

    return true;
}

const char *Parser::hook_moment_name(Parser::HookMoment moment)
{
    // the table is the one spelling, read backwards - the way the reader reads it forwards
    for (const auto &[spelled, row] : hook_moment_table()) {
        if (row == moment) {
            return spelled.c_str();
        }
    }

    return "";
}
