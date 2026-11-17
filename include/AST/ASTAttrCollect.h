#ifndef ASTATTRCOLLECT_H
#define ASTATTRCOLLECT_H

#pragma once

#include "AST/ASTValueType.h"
#include "Token.h"

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AST
{
    class Bundle;
    class Collector;
    class CoreTypes;
    class AttributeNode;
    class FunctionCallExprNode;
    class FunctionDeclNode;
    class TypeDeclNode;
    struct AttributeValue;

    enum class AttrSubjectKind
    {
        t_function,
        t_type,
    };

    struct AttrMatch
    {
        AttrSubjectKind kind = AttrSubjectKind::t_function;
        AttributeNode *attribute = nullptr;
        FunctionDeclNode *function = nullptr;
        TypeDeclNode *type = nullptr;
    };

    enum class AttrTargetKind
    {
        t_none,
        t_callable,
        t_c_function,
        t_type_id,
    };

    enum class AttrPlanStatus
    {
        t_ready,
        t_refused,
    };

    struct AttrSchema
    {
        ValueType row_type = ValueType::make_unknown();
        AttrTargetKind target_kind = AttrTargetKind::t_none;
        size_t target_index = 0;
        ValueType target_type = ValueType::make_unknown();
        std::vector<size_t> payload_indexes;
        std::string refusal;
    };

    // one intern-resolved payload field. codegen lowers this; it does not re-walk the attribute
    struct AttrPayloadSlot
    {
        size_t index = 0;
        ValueType type = ValueType::make_unknown();
        const AttributeValue *value = nullptr;
    };

    struct AttrRow
    {
        AttrMatch match;
        std::string key;
        std::vector<AttrPayloadSlot> payload;
    };

    // one attr::of call's $key argument. intern is append-only; check_attr_collects owns identity
    struct AttrKeyRequest
    {
        std::string field;
        bool folded = true;
        std::optional<TokenReference> at;
    };

    struct AttrPlan
    {
        AttrPlanStatus status = AttrPlanStatus::t_refused;
        std::string attribute_name;
        std::string key_field;
        std::vector<AttrKeyRequest> key_requests;
        AttrSchema schema;
        ValueType table_type = ValueType::make_unknown();
        std::vector<AttrRow> rows;

        // **the table each root of a several-roots test run sees**, as indexes into `rows`, one entry
        // per AST::Bundle::root_programs in that order. a root's own build collects from its own
        // program, in its own parse order, and this is that answer kept beside the bundle-wide one.
        // empty in every other build, where `rows` is the table
        std::vector<std::vector<size_t>> views;

        std::string refusal;
        std::optional<TokenReference> at;
    };

    // intern table, forward-declarable so CodegenContext does not include this header
    class AttrPlanTable
    {
    public:
        AttrPlan *find(const std::string &intern_key)
        {
            auto it = entries.find(intern_key);
            return it == entries.end() ? nullptr : &it->second;
        }

        const AttrPlan *find(const std::string &intern_key) const
        {
            auto it = entries.find(intern_key);
            return it == entries.end() ? nullptr : &it->second;
        }

        AttrPlan *intern(std::string intern_key, AttrPlan plan)
        {
            auto [it, _] = entries.emplace(std::move(intern_key), std::move(plan));
            return &it->second;
        }

        // call-site refusal with no intern key: a name that has not folded
        AttrPlan *store_call_refusal(AttrPlan plan)
        {
            call_refusals.push_back(std::move(plan));
            return &call_refusals.back();
        }

        std::unordered_map<std::string, AttrPlan> entries;
        std::deque<AttrPlan> call_refusals;

        // the root module each AttrPlan::views entry answers for, by name. codegen selects a view by
        // the module of the test it is about to call, and has no bundle to ask
        std::vector<std::string> view_roots;
    };

    std::vector<AttrMatch> declarations_with_attribute(const Bundle &bundle, std::string_view name);

    bool is_attr_of_builtin(const FunctionDeclNode *decl);

    // intern key for AttrPlanTable::entries: attribute name plus the schema type
    std::string attr_plan_key(std::string_view name, const ValueType &row);

    // **the shape of this `attr::of<T>` call.** interned on the collector by attr_plan_key.
    // nullptr while the builtin is still generic. a name that has not folded is a refused plan,
    // not nullptr. rows are filled by check_attr_collects
    AttrPlan *attr_plan_for(FunctionCallExprNode &call, Collector &collector);

    // read-only intern lookup for codegen. nullptr when the name has not folded
    const AttrPlan *attr_plan_lookup(const FunctionCallExprNode &call, const AttrPlanTable &plans);

    AttrSchema attr_schema_of(const ValueType &row_type, const CoreTypes &core);

    std::string attr_table_symbol(const AttrPlan &plan);

    // every index into `rows`: the view of a build that has no AttrPlan::views
    std::vector<size_t> attr_all_rows(const AttrPlan &plan);

    // rows, every tag refusal, duplicate keys, two schemas for one name, two key fields. duplicate
    // keys are asked of each root's view when the bundle holds several root programs: two roots may
    // each register a name the other never sees
    void check_attr_collects(Collector &collector, const Bundle &bundle);
};

#endif
