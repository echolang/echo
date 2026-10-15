#include "Parser/ManifestTargets.h"

#include "Compiler/LinkRequirement.h"

#include <fmt/core.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace
{

// what one kind's *record* is read against, so the shape rules are stated once per kind rather than
// re-derived at each field. `exe` owes an entry file and a `test` is refused for writing one
struct TargetKindShape
{
    std::vector<std::string> fields;
    std::vector<std::string> required;
    std::optional<std::string> default_name;
};

// the kinds a target may produce. **one row per spelling**, read through AttributeReader::tag, so a
// misspelled kind refuses in the same words a misspelled `#[link:]` scheme does
const std::vector<std::pair<std::string, Parser::TargetKind>> &target_kind_table()
{
    static const std::vector<std::pair<std::string, Parser::TargetKind>> table = {
        { "exe", Parser::TargetKind::t_executable },
        { "test", Parser::TargetKind::t_test },
        { "lib", Parser::TargetKind::t_library },
    };

    return table;
}

const TargetKindShape &target_kind_shape(Parser::TargetKind kind)
{
    static const TargetKindShape executable = {
        { "name", "entry", "install", "public" },
        { "name", "entry" },
        std::nullopt
    };

    static const TargetKindShape test = {
        { "name", "groups", "files" },
        {},
        std::string("tests")
    };

    static const TargetKindShape library = {
        { "name" },
        { "name" },
        std::nullopt
    };

    switch (kind) {
    case Parser::TargetKind::t_executable:
        return executable;
    case Parser::TargetKind::t_test:
        return test;
    case Parser::TargetKind::t_library:
        return library;
    }

    return executable;
}

};

void Parser::read_manifest_targets(
    const AST::AttributeValue &written,
    AST::AttributeReader &reader,
    uint32_t line,
    std::vector<Parser::WrittenTarget> &out_targets)
{
    for (const AST::AttributeValue *entry : AST::AttributeReader::each(written)) {
        Parser::TargetKind kind = Parser::TargetKind::t_executable;

        if (entry->has_tag()
            && !reader.tag(
                *entry, target_kind_table(), "target kind", Compiler::scheme_list_of(target_kind_table()),
                kind)) {
            continue;
        }

        if (!entry->has_tag() && entry->is(AST::AttributeValueKind::t_name)) {
            const auto found = std::find_if(
                target_kind_table().begin(), target_kind_table().end(),
                [entry](const auto &row) { return row.first == entry->text; });

            if (found != target_kind_table().end()) {
                const TargetKindShape &shape = target_kind_shape(found->second);

                if (!shape.default_name.has_value()) {
                    reader.refuse(entry->span, fmt::format(
                        "a '{}' target has nothing it can default - write its fields: {}.",
                        entry->text, fmt::join(shape.fields, ", ")));
                    continue;
                }

                Parser::WrittenTarget bare;
                bare.name = shape.default_name.value();
                bare.kind = found->second;
                bare.line = entry->span.is_valid() ? entry->span.line() : line;

                out_targets.push_back(std::move(bare));
                continue;
            }
        }

        const TargetKindShape &shape = target_kind_shape(kind);

        for (const AST::AttributeValue *record : AST::AttributeReader::payload(*entry)) {
            if (!reader.record(*record)) {
                continue;
            }

            bool complete = true;

            for (const std::string &required : shape.required) {
                if (reader.require_field(*record, required) == nullptr) {
                    complete = false;
                }
            }

            reader.reject_unknown_fields(*record, shape.fields);

            if (!complete) {
                continue;
            }

            Parser::WrittenTarget target;
            target.kind = kind;
            target.line = record->span.is_valid() ? record->span.line() : line;

            if (const AST::AttributeValue *name = reader.field(*record, "name")) {
                std::optional<std::string> spelled = reader.string(*name);

                if (!spelled.has_value()) {
                    continue;
                }

                target.name = spelled.value();
            }
            else {
                target.name = shape.default_name.value_or("");
            }

            if (const AST::AttributeValue *entry_file = reader.field(*record, "entry")) {
                std::optional<std::string> spelled = reader.string(*entry_file);

                if (!spelled.has_value()) {
                    continue;
                }

                target.entry = spelled.value();
            }

            if (const AST::AttributeValue *install = reader.field(*record, "install")) {
                std::optional<bool> spelled = reader.boolean(*install);

                if (!spelled.has_value()) {
                    continue;
                }

                target.install = spelled.value();
            }

            if (const AST::AttributeValue *offered = reader.field(*record, "public")) {
                std::optional<bool> spelled = reader.boolean(*offered);

                if (!spelled.has_value()) {
                    continue;
                }

                target.is_public = spelled.value();
            }

            const auto read_strings = [&reader, record](
                const std::string &key, std::vector<std::string> &into) {
                const AST::AttributeValue *field = reader.field(*record, key);

                if (field == nullptr) {
                    return;
                }

                for (const AST::AttributeValue *item : AST::AttributeReader::each(*field)) {
                    if (std::optional<std::string> spelled = reader.string(*item)) {
                        into.push_back(spelled.value());
                    }
                }
            };

            read_strings("groups", target.groups);
            read_strings("files", target.files);

            out_targets.push_back(std::move(target));
        }
    }
}
