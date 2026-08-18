#include "Parser/ManifestPackage.h"

#include "Compiler/SettledPath.h"

#include <fmt/core.h>

#include <filesystem>
#include <string_view>
#include <system_error>

bool Parser::package_name_is_usable(const std::string &name)
{
    if (name.empty() || name.find('\\') != std::string::npos) {
        return false;
    }

    int components = 0;
    size_t start = 0;

    while (start <= name.size()) {
        const size_t slash = name.find('/', start);
        const size_t end = slash == std::string::npos ? name.size() : slash;
        const std::string_view part(name.data() + start, end - start);

        if (part.empty() || part == "." || part == "..") {
            return false;
        }

        components += 1;

        if (components > k_max_package_name_depth) {
            return false;
        }

        if (slash == std::string::npos) {
            break;
        }

        start = slash + 1;
    }

    return components >= 1;
}

void Parser::read_manifest_requires(
    const AST::AttributeValue &written,
    AST::AttributeReader &reader,
    std::vector<Parser::ModuleRequirement> &out_requirements
)
{
    for (const AST::AttributeValue *entry : AST::AttributeReader::each(written)) {
        if (!entry->has_tag() || !entry->is(AST::AttributeValueKind::t_record)) {
            reader.refuse(entry->span,
                "a requirement is '\"name\" { version:, source: git \"...\", rev: }'.");
            continue;
        }

        if (!reader.record(*entry)) {
            continue;
        }

        const std::string name = entry->tag();

        if (!package_name_is_usable(name)) {
            reader.refuse(entry->tag_span, fmt::format(
                "'{}' is not a usable package name - it becomes a directory under 'vendor/', so "
                "each component must be a real segment (not '.', '..', empty, or a backslash).",
                name));
            continue;
        }

        bool duplicate = false;

        for (const Parser::ModuleRequirement &existing : out_requirements) {
            if (existing.name != name) {
                continue;
            }

            reader.refuse(entry->tag_span, fmt::format(
                "'{}' is required twice - two versions of one package cannot coexist.", name));
            duplicate = true;
            break;
        }

        if (duplicate) {
            continue;
        }

        const AST::AttributeValue *version = reader.require_field(*entry, "version");
        const AST::AttributeValue *source = reader.require_field(*entry, "source");
        reader.reject_unknown_fields(*entry, { "version", "source", "rev" });

        if (reader.has_refusals() || version == nullptr || source == nullptr) {
            continue;
        }

        Parser::ModuleRequirement requirement;
        requirement.name = name;
        requirement.span = entry->span;

        if (std::optional<std::string> value = reader.string(*version)) {
            requirement.version = value.value();
        }

        Parser::RequirementSourceKind kind = Parser::RequirementSourceKind::t_git;
        static const std::vector<std::pair<std::string, Parser::RequirementSourceKind>> kinds = {
            { "git", Parser::RequirementSourceKind::t_git }
        };

        if (reader.tag(*source, kinds, "source", "git", kind)) {
            requirement.source_kind = kind;

            if (std::optional<std::string> value = reader.string(*source)) {
                requirement.source = value.value();
            }
        }

        if (const AST::AttributeValue *rev = reader.field(*entry, "rev")) {
            if (std::optional<std::string> value = reader.string(*rev)) {
                requirement.rev = value.value();
            }
        }

        if (reader.has_refusals()) {
            continue;
        }

        out_requirements.push_back(std::move(requirement));
    }
}

std::optional<std::filesystem::path> Parser::manifest_for_requirement(
    const Parser::ModuleRequirement &requirement,
    const std::filesystem::path &package_dir
)
{
    return Parser::manifest_at(package_dir / requirement.name);
}

std::filesystem::path Parser::resolve_package_dir(
    const std::filesystem::path &entry_directory,
    const std::filesystem::path &override_dir
)
{
    if (!override_dir.empty()) {
        return Compiler::canonical_or_absolute(override_dir);
    }

    if (entry_directory.empty()) {
        return {};
    }

    std::error_code ec;

    // standing inside `vendor/<name>`: the package dir is the ancestor named vendor, so
    // `cd vendor/libcurl && echoc test` and `cd vendor/echolang/libcurl && echoc test`
    // both still see the project's other packages. the walk is bounded by the same
    // depth a package name may have, and it stops at another `module.eco` so a project
    // that happens to sit under a directory named vendor is not treated as a package
    std::filesystem::path here = entry_directory;

    for (int depth = 0; depth < k_max_package_name_depth; depth++) {
        const std::filesystem::path parent = here.parent_path();

        if (parent.empty() || parent == here) {
            break;
        }

        if (parent.filename() == "vendor" && std::filesystem::is_directory(parent, ec)) {
            return Compiler::canonical_or_absolute(parent);
        }

        here = parent;

        if (std::filesystem::is_regular_file(here / "module.eco", ec)) {
            break;
        }
    }

    return Compiler::canonical_or_absolute(entry_directory / "vendor");
}
