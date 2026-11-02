#include "AST/ASTAttrCollect.h"

#include "AST/ASTAttrSchema.h"
#include "AST/ASTAttributes.h"
#include "AST/ASTBuiltin.h"
#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "AST/ASTModule.h"
#include "AST/ASTValueType.h"
#include "AST/AttributeNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"
#include "Compiler/Fnv.h"
#include "Token.h"

#include <fmt/core.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{

struct RankedMatch
{
    size_t module_index = 0;
    std::string file;
    uint32_t line = 0;
    uint32_t column = 0;
    AST::AttrMatch match;
};

std::string file_key(const TokenReference &token)
{
    if (!token.is_valid() || token.file() == nullptr) {
        return {};
    }

    return token.file()->get_path().generic_string();
}

std::string attribute_key(const AST::AttributeNode &attribute)
{
    const TokenReference &token = attribute.attribute_id;
    if (!token.is_valid()) {
        return {};
    }

    return file_key(token) + ":" + std::to_string(token.line()) + ":"
        + std::to_string(token.column());
}

void rank_from_attribute(RankedMatch &row, const AST::AttributeNode &attribute)
{
    const TokenReference &token = attribute.attribute_id;
    row.file = file_key(token);
    if (token.is_valid()) {
        row.line = token.line();
        row.column = token.column();
    }
}

bool skip_function(const AST::FunctionDeclNode *fn)
{
    if (fn == nullptr) {
        return true;
    }

    return fn->is_closure
        || fn->is_implicitly_generated
        || fn->is_instantiated()
        || fn->is_test()
        || fn->is_init()
        || fn->defaulted_requirement != nullptr;
}

bool skip_type(const AST::TypeDeclNode *decl)
{
    if (decl == nullptr) {
        return true;
    }

    const AST::ComplexType &type = decl->complex_type();
    return type.is_optional || type.is_instantiated();
}

bool attribute_is_named_tool(const AST::AttributeNode &attribute, std::string_view name)
{
    if (attribute.attribute_id.value() != name) {
        return false;
    }

    return AST::tool_attribute_name(attribute.attribute_id.value()).has_value();
}

void append_function_matches(
    std::vector<RankedMatch> &into,
    std::unordered_set<std::string> &seen,
    size_t module_index,
    AST::FunctionDeclNode &fn,
    std::string_view name
)
{
    if (skip_function(&fn)) {
        return;
    }

    for (AST::AttributeNode *attribute : fn.attributes.all()) {
        if (attribute == nullptr || !attribute_is_named_tool(*attribute, name)) {
            continue;
        }

        if (!seen.insert(attribute_key(*attribute)).second) {
            continue;
        }

        RankedMatch row;
        row.module_index = module_index;
        rank_from_attribute(row, *attribute);
        row.match.kind = AST::AttrSubjectKind::t_function;
        row.match.attribute = attribute;
        row.match.function = &fn;
        into.push_back(std::move(row));
    }
}

void append_type_matches(
    std::vector<RankedMatch> &into,
    std::unordered_set<std::string> &seen,
    size_t module_index,
    AST::TypeDeclNode &decl,
    std::string_view name
)
{
    if (skip_type(&decl)) {
        return;
    }

    for (AST::AttributeNode *attribute : decl.attributes.all()) {
        if (attribute == nullptr || !attribute_is_named_tool(*attribute, name)) {
            continue;
        }

        if (!seen.insert(attribute_key(*attribute)).second) {
            continue;
        }

        RankedMatch row;
        row.module_index = module_index;
        rank_from_attribute(row, *attribute);
        row.match.kind = AST::AttrSubjectKind::t_type;
        row.match.attribute = attribute;
        row.match.type = &decl;
        into.push_back(std::move(row));
    }
}

bool ranked_before(const RankedMatch &a, const RankedMatch &b)
{
    if (a.module_index != b.module_index) {
        return a.module_index < b.module_index;
    }
    if (a.file != b.file) {
        return a.file < b.file;
    }
    if (a.line != b.line) {
        return a.line < b.line;
    }
    return a.column < b.column;
}

std::optional<std::string> folded_string_arg(const AST::FunctionCallExprNode &call, size_t index)
{
    if (index >= call.arguments.size() || call.arguments[index] == nullptr) {
        return std::nullopt;
    }

    return AST::literal_string_value(call.arguments[index]);
}

void record_key_request(AST::AttrPlan &plan, const AST::FunctionCallExprNode &call)
{
    if (call.arguments.size() <= 1) {
        return;
    }

    AST::AttrKeyRequest request;
    request.at.emplace(call.token_function_name);
    const std::optional<std::string> key = folded_string_arg(call, 1);
    if (!key.has_value()) {
        request.folded = false;
    } else {
        request.field = *key;
    }
    plan.key_requests.push_back(std::move(request));
}

bool plan_before(const AST::AttrPlan *a, const AST::AttrPlan *b)
{
    if (a->attribute_name != b->attribute_name) {
        return a->attribute_name < b->attribute_name;
    }

    const std::string file_a = a->at.has_value() ? file_key(*a->at) : std::string();
    const std::string file_b = b->at.has_value() ? file_key(*b->at) : std::string();
    if (file_a != file_b) {
        return file_a < file_b;
    }

    const uint32_t line_a = a->at.has_value() && a->at->is_valid() ? a->at->line() : 0;
    const uint32_t line_b = b->at.has_value() && b->at->is_valid() ? b->at->line() : 0;
    if (line_a != line_b) {
        return line_a < line_b;
    }

    const uint32_t column_a = a->at.has_value() && a->at->is_valid() ? a->at->column() : 0;
    const uint32_t column_b = b->at.has_value() && b->at->is_valid() ? b->at->column() : 0;
    if (column_a != column_b) {
        return column_a < column_b;
    }

    return a->schema.row_type.get_mangled_name() < b->schema.row_type.get_mangled_name();
}

void report_at(
    AST::Collector &collector,
    const AST::Module *module,
    const TokenReference &token,
    const std::string &message
)
{
    if (module == nullptr || !token.is_valid()) {
        return;
    }

    collector.collect_issue<AST::Issue::CannotCollectAttribute>(
        AST::CodeRef { module, token.make_slice() },
        message);
}

const TokenReference *row_site(const AST::AttrMatch &hit)
{
    if (hit.attribute != nullptr && hit.attribute->attribute_id.is_valid()) {
        return &hit.attribute->attribute_id;
    }
    if (hit.function != nullptr) {
        return &hit.function->declaration_site_token();
    }
    if (hit.type != nullptr) {
        return &hit.type->declaration_site_token();
    }
    return nullptr;
}

void fill_plan_rows(AST::AttrPlan &plan, const AST::Bundle &bundle, AST::Collector &collector)
{
    bool any_bad = false;

    for (const AST::AttrMatch &hit : AST::declarations_with_attribute(bundle, plan.attribute_name)) {
        if (hit.attribute == nullptr) {
            continue;
        }

        if (auto shape = AST::attr_payload_shape_refusal(
                plan.schema, *hit.attribute, collector.core_types)) {
            report_at(
                collector,
                hit.attribute->attribute_id.file() != nullptr
                    ? hit.attribute->attribute_id.file()->module
                    : nullptr,
                hit.attribute->attribute_id,
                *shape);
            any_bad = true;
            continue;
        }

        if (auto reason = AST::attr_row_refusal(hit, plan.schema)) {
            const TokenReference *at = nullptr;
            if (hit.function != nullptr) {
                at = &hit.function->declaration_site_token();
            } else if (hit.type != nullptr) {
                at = &hit.type->declaration_site_token();
            } else {
                at = &hit.attribute->attribute_id;
            }
            report_at(
                collector,
                at->file() != nullptr ? at->file()->module : nullptr,
                *at,
                *reason);
            any_bad = true;
            continue;
        }

        AST::AttrRow row;
        row.match = hit;
        AST::attr_bind_payload(row, plan.schema);
        plan.rows.push_back(std::move(row));
    }

    if (any_bad) {
        plan.status = AST::AttrPlanStatus::t_refused;
        plan.rows.clear();
        if (plan.refusal.empty()) {
            plan.refusal = "this attribute could not be collected";
        }
    }
}

}

std::vector<AST::AttrMatch> AST::declarations_with_attribute(
    const AST::Bundle &bundle,
    std::string_view name
)
{
    std::vector<RankedMatch> ranked;
    std::unordered_set<std::string> seen;
    size_t module_index = 0;

    for (const auto &module_ptr : bundle.modules) {
        if (module_ptr == nullptr) {
            ++module_index;
            continue;
        }

        AST::Module &module = *module_ptr;

        for (AST::FunctionDeclNode *fn : module.nodes.of_type<AST::FunctionDeclNode>()) {
            if (fn != nullptr) {
                append_function_matches(ranked, seen, module_index, *fn, name);
            }
        }

        for (AST::TypeDeclNode *decl : module.nodes.of_type<AST::TypeDeclNode>()) {
            if (decl != nullptr) {
                append_type_matches(ranked, seen, module_index, *decl, name);
            }
        }

        ++module_index;
    }

    std::sort(ranked.begin(), ranked.end(), ranked_before);

    std::vector<AST::AttrMatch> out;
    out.reserve(ranked.size());
    for (RankedMatch &row : ranked) {
        out.push_back(std::move(row.match));
    }

    return out;
}

bool AST::is_attr_of_builtin(const AST::FunctionDeclNode *decl)
{
    return decl != nullptr
        && decl->is_builtin()
        && decl->builtin.has_value()
        && AST::is_known_builtin(decl->builtin.value())
        && AST::builtin_kind_for(decl->builtin.value()) == AST::BuiltinKind::t_attr_of;
}

std::string AST::attr_plan_key(std::string_view name, const AST::ValueType &row)
{
    return std::string(name) + "\n" + row.get_mangled_name();
}

std::string AST::attr_table_symbol(const AST::AttrPlan &plan)
{
    uint64_t hash = Compiler::fnv1a64(plan.attribute_name, Compiler::k_fnv_offset_basis);
    hash = Compiler::fnv1a64(plan.schema.row_type.get_mangled_name(), hash);
    return "__eco_attr_" + Compiler::to_hex(hash);
}

AST::AttrPlan *AST::attr_plan_for(AST::FunctionCallExprNode &call, AST::Collector &collector)
{
    if (!is_attr_of_builtin(call.decl)) {
        return nullptr;
    }

    if (call.decl->is_generic() || call.decl->instantiation_args.size() != 1) {
        return nullptr;
    }

    const AST::ValueType row = call.decl->instantiation_args[0];
    const std::optional<std::string> name = folded_string_arg(call, 0);
    if (!name.has_value()) {
        AST::AttrPlan plan;
        plan.status = AST::AttrPlanStatus::t_refused;
        plan.refusal = "attr::of needs a string literal as its attribute name";
        plan.at.emplace(call.token_function_name);
        plan.table_type = call.decl->get_return_type();
        plan.schema = attr_schema_of(row, collector.core_types);
        return collector.attr_plans.store_call_refusal(std::move(plan));
    }

    const std::string intern = attr_plan_key(*name, row);
    if (AST::AttrPlan *found = collector.attr_plans.find(intern)) {
        record_key_request(*found, call);
        return found;
    }

    AST::AttrPlan plan;
    plan.attribute_name = *name;
    plan.at.emplace(call.token_function_name);
    plan.table_type = call.decl->get_return_type();
    plan.schema = attr_schema_of(row, collector.core_types);
    record_key_request(plan, call);

    if (name->empty()) {
        plan.status = AST::AttrPlanStatus::t_refused;
        plan.refusal = "attr::of needs an attribute name, not an empty string";
        return collector.attr_plans.intern(intern, std::move(plan));
    }

    if (!AST::tool_attribute_name(*name).has_value()) {
        plan.status = AST::AttrPlanStatus::t_refused;
        plan.refusal = fmt::format(
            "'{}' is not a tool attribute; attr::of collects names like 'http::get'",
            *name);
        return collector.attr_plans.intern(intern, std::move(plan));
    }

    if (!plan.schema.refusal.empty()) {
        plan.status = AST::AttrPlanStatus::t_refused;
        plan.refusal = plan.schema.refusal;
        return collector.attr_plans.intern(intern, std::move(plan));
    }

    plan.status = AST::AttrPlanStatus::t_ready;
    return collector.attr_plans.intern(intern, std::move(plan));
}

const AST::AttrPlan *AST::attr_plan_lookup(
    const AST::FunctionCallExprNode &call,
    const AST::AttrPlanTable &plans
)
{
    if (call.decl == nullptr || call.decl->instantiation_args.size() != 1) {
        return nullptr;
    }

    const std::optional<std::string> name = AST::literal_string_value(
        call.arguments.empty() ? nullptr : call.arguments[0]);
    if (!name.has_value()) {
        return nullptr;
    }

    return plans.find(attr_plan_key(*name, call.decl->instantiation_args[0]));
}

void AST::check_attr_collects(AST::Collector &collector, const AST::Bundle &bundle)
{
    std::vector<AST::AttrPlan *> ready;
    ready.reserve(collector.attr_plans.entries.size());
    for (auto &entry : collector.attr_plans.entries) {
        if (entry.second.status == AST::AttrPlanStatus::t_ready) {
            ready.push_back(&entry.second);
        }
    }
    std::sort(ready.begin(), ready.end(), plan_before);

    std::unordered_map<std::string, const AST::AttrPlan *> by_name;

    for (AST::AttrPlan *plan_ptr : ready) {
        AST::AttrPlan &plan = *plan_ptr;

        auto [it, inserted] = by_name.emplace(plan.attribute_name, &plan);
        if (!inserted && it->second->schema.row_type != plan.schema.row_type) {
            if (plan.at.has_value()) {
                report_at(
                    collector,
                    plan.at->file() != nullptr ? plan.at->file()->module : nullptr,
                    plan.at.value(),
                    fmt::format(
                        "attr::of collects '{}' as '{}', and another call collects it as '{}'",
                        plan.attribute_name,
                        plan.schema.row_type.get_type_desciption(),
                        it->second->schema.row_type.get_type_desciption()));
            }
            plan.status = AST::AttrPlanStatus::t_refused;
            continue;
        }

        fill_plan_rows(plan, bundle, collector);
        if (plan.status != AST::AttrPlanStatus::t_ready) {
            continue;
        }

        std::string chosen;
        bool key_ok = true;
        for (const AST::AttrKeyRequest &request : plan.key_requests) {
            const TokenReference *at = request.at.has_value() ? &request.at.value() : nullptr;
            if (at == nullptr && plan.at.has_value()) {
                at = &plan.at.value();
            }
            if (at == nullptr) {
                continue;
            }

            const AST::Module *module = at->file() != nullptr ? at->file()->module : nullptr;

            if (!request.folded) {
                report_at(
                    collector,
                    module,
                    *at,
                    "attr::of needs a string literal as its key field name");
                key_ok = false;
                continue;
            }

            if (request.field.empty()) {
                continue;
            }

            if (auto reason = AST::attr_key_field_refusal(
                    plan.schema, request.field, collector.core_types)) {
                report_at(collector, module, *at, *reason);
                key_ok = false;
                continue;
            }

            if (chosen.empty()) {
                chosen = request.field;
            } else if (chosen != request.field) {
                report_at(
                    collector,
                    module,
                    *at,
                    fmt::format(
                        "attr::of collects '{}' keyed on '{}', and another call keys it on '{}'",
                        plan.attribute_name,
                        chosen,
                        request.field));
                key_ok = false;
            }
        }

        if (!key_ok) {
            plan.status = AST::AttrPlanStatus::t_refused;
            continue;
        }

        if (chosen.empty()) {
            continue;
        }

        plan.key_field = chosen;
        const AST::ComplexType::Property *key_property =
            plan.schema.row_type.get_complex_type()->find_property(plan.key_field);
        if (key_property == nullptr) {
            continue;
        }

        std::unordered_map<std::string, const AST::AttrRow *> seen;
        bool unique = true;
        for (AST::AttrRow &row : plan.rows) {
            row.key = AST::attr_payload_field_text(*row.match.attribute, *key_property);

            auto [seen_it, is_new] = seen.emplace(row.key, &row);
            if (is_new) {
                continue;
            }

            unique = false;
            const TokenReference *token = row_site(row.match);
            if (token == nullptr && plan.at.has_value()) {
                token = &plan.at.value();
            }
            if (token == nullptr) {
                continue;
            }

            report_at(
                collector,
                token->file() != nullptr ? token->file()->module : nullptr,
                *token,
                fmt::format(
                    "'{}' is used twice as the '{}' of '{}'",
                    row.key,
                    plan.key_field,
                    plan.attribute_name));
        }

        if (!unique) {
            plan.status = AST::AttrPlanStatus::t_refused;
        }
    }
}
