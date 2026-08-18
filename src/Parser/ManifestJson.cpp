#include "Parser/ManifestParser.h"
#include "AST/ASTAttributeValue.h"

#include <fmt/core.h>

#include <filesystem>
#include <optional>
#include <ostream>
#include <sstream>

namespace
{

void json_escape(std::ostream &out, const std::string &text)
{
    out << '"';

    for (const unsigned char byte : text) {
        switch (byte) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (byte < 0x20) {
                out << fmt::format("\\u{:04x}", byte);
            }
            else {
                out << static_cast<char>(byte);
            }
            break;
        }
    }

    out << '"';
}

void json_optional_string(std::ostream &out, const std::string &text)
{
    if (text.empty()) {
        out << "null";
    }
    else {
        json_escape(out, text);
    }
}

void json_string_array(std::ostream &out, const std::vector<std::string> &items)
{
    out << '[';

    for (size_t i = 0; i < items.size(); i++) {
        if (i > 0) {
            out << ", ";
        }

        json_escape(out, items[i]);
    }

    out << ']';
}

void json_attribute_value(std::ostream &out, const AST::AttributeValue &value)
{
    out << "{ \"kind\": ";

    switch (value.kind) {
    case AST::AttributeValueKind::t_string:
        out << "\"string\", \"text\": ";
        json_escape(out, value.text);
        break;
    case AST::AttributeValueKind::t_int:
        out << "\"int\", \"integer\": " << value.integer;
        break;
    case AST::AttributeValueKind::t_float:
        out << "\"float\", \"number\": " << fmt::format("{}", value.number);
        break;
    case AST::AttributeValueKind::t_bool:
        out << "\"bool\", \"boolean\": " << (value.boolean ? "true" : "false");
        break;
    case AST::AttributeValueKind::t_name:
        out << "\"name\", \"text\": ";
        json_escape(out, value.text);
        break;
    case AST::AttributeValueKind::t_list:
        out << "\"list\", \"items\": [";
        for (size_t i = 0; i < value.items.size(); i++) {
            if (i > 0) {
                out << ", ";
            }
            json_attribute_value(out, value.items[i]);
        }
        out << ']';
        break;
    case AST::AttributeValueKind::t_record:
        out << "\"record\", \"fields\": [";
        for (size_t i = 0; i < value.fields.size(); i++) {
            if (i > 0) {
                out << ", ";
            }
            out << "{ \"key\": ";
            json_escape(out, value.fields[i].key);
            out << ", \"value\": ";
            json_attribute_value(out, value.fields[i].value);
            out << " }";
        }
        out << ']';
        break;
    }

    if (value.has_tag()) {
        out << ", \"tag\": ";
        json_escape(out, value.tag());
    }

    out << " }";
}

const char *requirement_source_kind_name(Parser::RequirementSourceKind kind)
{
    switch (kind) {
    case Parser::RequirementSourceKind::t_git: return "git";
    }

    return "git";
}

void json_requirements(std::ostream &out, const std::vector<Parser::ModuleRequirement> &requirements)
{
    out << '[';

    for (size_t i = 0; i < requirements.size(); i++) {
        if (i > 0) {
            out << ", ";
        }

        const Parser::ModuleRequirement &requirement = requirements[i];
        out << "{ \"name\": ";
        json_escape(out, requirement.name);
        out << ", \"version\": ";
        json_escape(out, requirement.version);
        out << ", \"source\": { \"kind\": ";
        json_escape(out, requirement_source_kind_name(requirement.source_kind));
        out << ", \"url\": ";
        json_escape(out, requirement.source);
        out << " }, \"rev\": ";
        json_optional_string(out, requirement.rev);
        out << " }";
    }

    out << ']';
}

const char *target_kind_name(Parser::TargetKind kind)
{
    switch (kind) {
    case Parser::TargetKind::t_executable: return "exe";
    case Parser::TargetKind::t_test: return "test";
    }

    return "exe";
}

};

std::string Parser::manifest_as_json(const Parser::ModuleManifest &manifest)
{
    std::ostringstream out;

    out << "{\n";
    out << "  \"name\": ";
    json_escape(out, manifest.name);
    out << ",\n  \"version\": ";
    json_escape(out, manifest.version);
    out << ",\n  \"depends\": ";
    json_string_array(out, manifest.depends_as_written);
    out << ",\n  \"requires\": ";
    json_requirements(out, manifest.requirements);
    out << ",\n  \"sources\": ";
    json_string_array(out, manifest.sources_as_written);
    out << ",\n  \"targets\": [";

    for (size_t i = 0; i < manifest.targets.size(); i++) {
        if (i > 0) {
            out << ", ";
        }

        const Parser::ModuleTarget &target = manifest.targets[i];
        out << "{ \"name\": ";
        json_escape(out, target.name);
        out << ", \"kind\": ";
        json_escape(out, target_kind_name(target.kind));
        out << ", \"requires\": ";
        json_requirements(out, target.requirements);
        out << ", \"depends\": ";
        json_string_array(out, target.depends_as_written);
        out << ", \"sources\": ";
        json_string_array(out, target.sources_as_written);
        out << " }";
    }

    out << "],\n  \"tools\": [";

    for (size_t i = 0; i < manifest.tools.size(); i++) {
        if (i > 0) {
            out << ", ";
        }

        const Parser::ToolAttribute &tool = manifest.tools[i];
        out << "{ \"namespace\": ";
        json_escape(out, tool.ns);
        out << ", \"name\": ";
        json_escape(out, tool.name);
        out << ", \"value\": ";
        json_attribute_value(out, tool.value);
        out << " }";
    }

    out << "]\n}\n";
    return out.str();
}

std::string Parser::manifests_as_json(const std::vector<Parser::ModuleManifest> &manifests)
{
    if (manifests.size() == 1) {
        return Parser::manifest_as_json(manifests.front());
    }

    std::ostringstream out;
    out << "[\n";

    for (size_t i = 0; i < manifests.size(); i++) {
        if (i > 0) {
            out << ",\n";
        }

        std::string one = Parser::manifest_as_json(manifests[i]);

        if (!one.empty() && one.back() == '\n') {
            one.pop_back();
        }

        out << one;
    }

    out << "\n]\n";
    return out.str();
}

std::optional<std::string> Parser::written_manifests_json(
    const std::vector<std::filesystem::path> &named,
    Parser::ManifestScratch &scratch,
    std::optional<std::filesystem::path> &out_missing
)
{
    out_missing.reset();
    std::vector<Parser::ModuleManifest> manifests;

    for (const std::filesystem::path &path : named) {
        const std::optional<std::filesystem::path> resolved = Parser::manifest_at(path);

        if (!resolved.has_value()) {
            out_missing = path;
            return std::nullopt;
        }

        Parser::ModuleManifest manifest;

        if (!Parser::read_module_manifest(
                resolved.value(), scratch, manifest, Parser::ManifestRead::t_written)) {
            return std::nullopt;
        }

        manifests.push_back(std::move(manifest));
    }

    return Parser::manifests_as_json(manifests);
}
