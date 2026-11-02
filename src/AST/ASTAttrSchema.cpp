#include "AST/ASTAttrSchema.h"

#include "AST/ASTAttributeValue.h"
#include "AST/ASTCFunction.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTValueType.h"
#include "AST/AttributeNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"

#include <fmt/core.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace
{

bool is_type_id_type(const AST::ValueType &type, const AST::CoreTypes &core)
{
    const AST::ValueType id = core.type(AST::CoreTypeKind::t_type_id);
    return !id.is_unknown() && type == id;
}

bool is_collect_target(const AST::ValueType &type, const AST::CoreTypes &core)
{
    return type.is_callable() || type.is_c_function() || is_type_id_type(type, core);
}

bool is_core_of(const AST::ValueType &type, AST::CoreTypeKind kind, const AST::CoreTypes &core)
{
    const AST::ComplexType *tmpl = core.declared_template(kind);
    return tmpl != nullptr
        && type.has_complex_type()
        && type.get_complex_type()->template_or_self() == tmpl;
}

bool payload_is_scalar(const AST::ValueType &field, const AST::CoreTypes &core)
{
    return core.is_string(field) || field.is_integer_type()
        || field.is_boolean_type() || field.is_floating_type();
}

std::optional<std::string> payload_field_type_refusal(
    const AST::ValueType &field,
    const AST::CoreTypes &core,
    std::string_view field_name
);

std::string value_key_text(const AST::AttributeValue &value)
{
    if (value.kind == AST::AttributeValueKind::t_string || value.kind == AST::AttributeValueKind::t_name) {
        return value.text;
    }
    return AST::attribute_value_spelling(value);
}

bool integer_fits_field(int64_t value, const AST::ValueType &field)
{
    const AST::IntegerSize size = AST::get_integer_size(field.get_primitive_type());
    if (value < 0) {
        return size.is_signed && value >= size.get_max_negative_value();
    }
    return static_cast<uint64_t>(value) <= size.get_max_positive_value();
}

std::optional<std::string> payload_field_refusal(
    const AST::AttributeValue &value,
    const AST::ValueType &field,
    const AST::CoreTypes &core,
    std::string_view attr_name,
    std::string_view field_name
)
{
    auto mismatch = [&](const char *what) {
        return fmt::format(
            "'{}' fills '{}' with {}, and '{}' is '{}'",
            attr_name,
            field_name,
            what,
            field_name,
            field.get_type_desciption());
    };

    switch (value.kind) {
    case AST::AttributeValueKind::t_string:
    case AST::AttributeValueKind::t_name:
        if (!core.is_string(field)) {
            return mismatch("a string");
        }
        return std::nullopt;
    case AST::AttributeValueKind::t_int:
        if (!field.is_integer_type()) {
            return mismatch("a number");
        }
        if (!integer_fits_field(value.integer, field)) {
            return fmt::format(
                "{} does not fit in '{}' of '{}'",
                value.integer,
                field_name,
                field.get_type_desciption());
        }
        return std::nullopt;
    case AST::AttributeValueKind::t_bool:
        if (!field.is_boolean_type()) {
            return mismatch("true or false");
        }
        return std::nullopt;
    case AST::AttributeValueKind::t_float:
        if (!field.is_floating_type()) {
            return mismatch("a float");
        }
        return std::nullopt;
    case AST::AttributeValueKind::t_list: {
        if (is_core_of(field, AST::CoreTypeKind::t_array, core)) {
            return fmt::format(
                "'{}' is '{}', and a collected list needs a slice or a T[N]",
                field_name,
                field.get_type_desciption());
        }
        const AST::ValueType *element = nullptr;
        if (auto layout = AST::attr_slice_layout(field)) {
            element = &layout->data->type.pointee();
        } else if (field.is_inline_array()) {
            const std::optional<uint64_t> n = field.bound_array_length();
            if (!n.has_value()) {
                return mismatch("a list");
            }
            if (static_cast<uint64_t>(value.items.size()) != *n) {
                return fmt::format(
                    "this list has {} elements, and '{}' is '{}'",
                    value.items.size(),
                    field_name,
                    field.get_type_desciption());
            }
            element = &field.array_element();
        } else {
            return mismatch("a list");
        }

        for (const AST::AttributeValue &item : value.items) {
            if (auto reason = payload_field_refusal(
                    item, *element, core, attr_name, field_name)) {
                return reason;
            }
        }
        return std::nullopt;
    }
    case AST::AttributeValueKind::t_record: {
        if (is_core_of(field, AST::CoreTypeKind::t_map, core)) {
            return fmt::format(
                "'{}' is '{}', and a collected record fills a struct",
                field_name,
                field.get_type_desciption());
        }

        if (core.is_string(field) || AST::attr_slice_layout(field).has_value()
            || is_core_of(field, AST::CoreTypeKind::t_array, core)
            || !field.is_struct() || !field.has_property_layout()) {
            return mismatch("a record");
        }

        const AST::ComplexType *nested = field.get_complex_type();
        std::unordered_set<std::string> wanted;
        for (size_t i = 0; i < nested->property_count(); i++) {
            const AST::ComplexType::Property &property = nested->get_property(i);
            if (is_collect_target(property.type, core)) {
                return fmt::format(
                    "a nested payload struct cannot hold a collected target, and '{}' does",
                    nested->name.value_or(std::string(field_name)));
            }
            wanted.insert(property.name);
        }

        for (const AST::AttributeField &entry : value.fields) {
            if (wanted.count(entry.key) == 0) {
                return fmt::format(
                    "'{}' has no field named '{}' to fill from this attribute",
                    nested->name.value_or(std::string(field_name)),
                    entry.key);
            }
        }

        for (size_t i = 0; i < nested->property_count(); i++) {
            const AST::ComplexType::Property &property = nested->get_property(i);
            const AST::AttributeField *named = value.find_field(property.name);
            if (named == nullptr) {
                return fmt::format(
                    "'{}' is missing from this '{}' attribute",
                    property.name,
                    attr_name);
            }
            if (auto reason = payload_field_refusal(
                    named->value, property.type, core, attr_name, property.name)) {
                return reason;
            }
        }
        return std::nullopt;
    }
    }

    return std::nullopt;
}

std::optional<std::string> payload_field_type_refusal(
    const AST::ValueType &field,
    const AST::CoreTypes &core,
    std::string_view field_name
)
{
    if (field.is_nullable()) {
        return fmt::format(
            "attr::of<T> cannot fill '{}' of type '{}'",
            field_name,
            field.get_type_desciption());
    }

    if (payload_is_scalar(field, core)) {
        return std::nullopt;
    }

    if (is_core_of(field, AST::CoreTypeKind::t_array, core)
        || is_core_of(field, AST::CoreTypeKind::t_map, core)) {
        return fmt::format(
            "attr::of<T> cannot fill '{}' of type '{}'",
            field_name,
            field.get_type_desciption());
    }

    if (auto layout = AST::attr_slice_layout(field)) {
        return payload_field_type_refusal(
            layout->data->type.pointee(), core, field_name);
    }

    if (field.is_inline_array()) {
        return payload_field_type_refusal(field.array_element(), core, field_name);
    }

    if (!field.is_struct() || !field.has_property_layout()) {
        return fmt::format(
            "attr::of<T> cannot fill '{}' of type '{}'",
            field_name,
            field.get_type_desciption());
    }

    const AST::ComplexType *nested = field.get_complex_type();
    for (size_t i = 0; i < nested->property_count(); i++) {
        const AST::ComplexType::Property &property = nested->get_property(i);
        if (is_collect_target(property.type, core)) {
            return fmt::format(
                "a nested payload struct cannot hold a collected target, and '{}' does",
                nested->name.value_or(std::string(field_name)));
        }
        if (auto reason = payload_field_type_refusal(property.type, core, property.name)) {
            return reason;
        }
    }

    return std::nullopt;
}

std::optional<std::string> function_target_refusal(
    const AST::FunctionDeclNode &fn,
    const AST::AttrSchema &schema
)
{
    if (fn.has_receiver()) {
        return std::string(
            "a method has a receiver this callable field does not take");
    }

    if (fn.is_generic()) {
        return std::string("a generic function has no single instance to collect");
    }

    if (schema.target_kind == AST::AttrTargetKind::t_callable) {
        const AST::ValueType have = fn.callable_type();
        if (have != schema.target_type) {
            return fmt::format(
                "this function is '{}', and the collected field wants '{}'",
                have.get_type_desciption(),
                schema.target_type.get_type_desciption());
        }
        return std::nullopt;
    }

    if (auto reason = AST::c_function_ref_refusal(fn)) {
        return reason;
    }

    if (!AST::c_function_signatures_match(
            fn.c_function_type().signature(), schema.target_type.signature())) {
        return fmt::format(
            "this function is '{}', and the collected field wants '{}'",
            fn.c_function_type().get_type_desciption(),
            schema.target_type.get_type_desciption());
    }

    return std::nullopt;
}

}

std::optional<AST::AttrSliceLayout> AST::attr_slice_layout(const AST::ValueType &field)
{
    if (!field.is_struct() || !field.has_complex_type()) {
        return std::nullopt;
    }

    const AST::ComplexType::Property *data = field.get_complex_type()->find_property("data");
    const AST::ComplexType::Property *len = field.get_complex_type()->find_property("len");
    if (data == nullptr || len == nullptr
        || !data->type.is_pointer() || !len->type.is_integer_type()) {
        return std::nullopt;
    }

    AST::AttrSliceLayout layout;
    layout.data = data;
    layout.len = len;
    return layout;
}

AST::AttrSchema AST::attr_schema_of(const AST::ValueType &row_type, const AST::CoreTypes &core)
{
    AST::AttrSchema schema;
    schema.row_type = row_type;

    if (!row_type.is_struct() || !row_type.has_complex_type()) {
        schema.refusal = "attr::of<T> needs T to be a struct";
        return schema;
    }

    const AST::ComplexType *ct = row_type.get_complex_type();
    size_t targets = 0;

    for (size_t i = 0; i < ct->property_count(); i++) {
        const AST::ComplexType::Property &property = ct->get_property(i);
        AST::AttrTargetKind kind = AST::AttrTargetKind::t_none;

        if (property.type.is_callable()) {
            kind = AST::AttrTargetKind::t_callable;
        } else if (property.type.is_c_function()) {
            kind = AST::AttrTargetKind::t_c_function;
        } else if (is_type_id_type(property.type, core)) {
            kind = AST::AttrTargetKind::t_type_id;
        }

        if (kind == AST::AttrTargetKind::t_none) {
            schema.payload_indexes.push_back(i);
            continue;
        }

        targets += 1;
        schema.target_kind = kind;
        schema.target_index = i;
        schema.target_type = property.type;
    }

    if (targets == 0) {
        schema.refusal =
            "attr::of<T> needs one target field: a function<...>, an extern function<...>, or a type_id";
        return schema;
    }

    if (targets > 1) {
        schema.refusal = "attr::of<T> needs exactly one target field, and this struct has more than one";
        return schema;
    }

    for (size_t index : schema.payload_indexes) {
        const AST::ComplexType::Property &property = ct->get_property(index);
        if (auto reason = payload_field_type_refusal(property.type, core, property.name)) {
            schema.refusal = *reason;
            return schema;
        }
    }

    return schema;
}

std::optional<std::string> AST::attr_payload_shape_refusal(
    const AST::AttrSchema &schema,
    const AST::AttributeNode &attribute,
    const AST::CoreTypes &core
)
{
    const AST::ComplexType *ct = schema.row_type.get_complex_type();
    if (ct == nullptr) {
        return std::string("attr::of<T> needs T to be a struct");
    }

    if (!attribute.value.has_value()) {
        if (!schema.payload_indexes.empty()) {
            return fmt::format(
                "'{}' needs a value to fill {} of '{}'",
                attribute.attribute_id.value(),
                schema.payload_indexes.size() == 1 ? "the payload field" : "the payload fields",
                ct->name.value_or("T"));
        }
        return std::nullopt;
    }

    const AST::AttributeValue &value = attribute.value.value();

    if (value.has_tag()) {
        return std::string("a collected attribute cannot be tagged");
    }

    if (schema.payload_indexes.empty()) {
        return fmt::format(
            "'{}' has a value, and '{}' has no payload field to fill",
            attribute.attribute_id.value(),
            ct->name.value_or("T"));
    }

    if (value.kind == AST::AttributeValueKind::t_record) {
        std::unordered_set<std::string> wanted;
        for (size_t index : schema.payload_indexes) {
            wanted.insert(ct->get_property(index).name);
        }

        for (const AST::AttributeField &field : value.fields) {
            if (wanted.count(field.key) == 0) {
                return fmt::format(
                    "'{}' has no field named '{}' to fill from this attribute",
                    ct->name.value_or("T"),
                    field.key);
            }
        }

        for (size_t index : schema.payload_indexes) {
            const AST::ComplexType::Property &property = ct->get_property(index);
            const AST::AttributeField *named = value.find_field(property.name);
            if (named == nullptr) {
                return fmt::format(
                    "'{}' is missing from this '{}' attribute",
                    property.name,
                    attribute.attribute_id.value());
            }
            if (auto reason = payload_field_refusal(
                    named->value,
                    property.type,
                    core,
                    attribute.attribute_id.value(),
                    property.name)) {
                return reason;
            }
        }

        return std::nullopt;
    }

    if (schema.payload_indexes.size() != 1) {
        return fmt::format(
            "'{}' wants a record {{ ... }} when '{}' has more than one payload field",
            attribute.attribute_id.value(),
            ct->name.value_or("T"));
    }

    const AST::ComplexType::Property &property = ct->get_property(schema.payload_indexes[0]);
    return payload_field_refusal(
        value, property.type, core, attribute.attribute_id.value(), property.name);
}

std::optional<std::string> AST::attr_row_refusal(
    const AST::AttrMatch &hit,
    const AST::AttrSchema &schema
)
{
    const bool collects_functions = schema.target_kind == AST::AttrTargetKind::t_callable
        || schema.target_kind == AST::AttrTargetKind::t_c_function;

    if (hit.kind == AST::AttrSubjectKind::t_function && hit.function != nullptr) {
        if (!collects_functions) {
            return std::string("this attribute collects types, not functions");
        }
        return function_target_refusal(*hit.function, schema);
    }

    if (hit.kind == AST::AttrSubjectKind::t_type && hit.type != nullptr) {
        if (collects_functions) {
            return std::string("this attribute collects functions, not types");
        }

        const AST::ComplexType &type = hit.type->complex_type();
        if (type.is_generic() && !type.is_instantiated()) {
            return std::string("a generic type has no single identity to collect");
        }
        return std::nullopt;
    }

    return std::nullopt;
}

std::optional<std::string> AST::attr_key_field_refusal(
    const AST::AttrSchema &schema,
    std::string_view key_field,
    const AST::CoreTypes &core
)
{
    if (key_field.empty()) {
        return std::nullopt;
    }

    const AST::ComplexType *ct = schema.row_type.get_complex_type();
    if (ct == nullptr) {
        return std::string("attr::of<T> needs T to be a struct");
    }

    const AST::ComplexType::Property *property = ct->find_property(std::string(key_field));
    if (property == nullptr) {
        return fmt::format(
            "'{}' has no field named '{}' to use as a unique key",
            schema.row_type.get_type_desciption(),
            key_field);
    }

    if (property->index == schema.target_index) {
        return fmt::format(
            "'{}' is the collected target of '{}', not a payload field to key on",
            key_field,
            ct->name.value_or("T"));
    }

    if (!payload_is_scalar(property->type, core)) {
        return fmt::format(
            "'{}' is '{}', not a scalar payload field to key on",
            key_field,
            property->type.get_type_desciption());
    }

    return std::nullopt;
}

std::string AST::attr_payload_field_text(
    const AST::AttributeNode &attribute,
    const AST::ComplexType::Property &property
)
{
    if (!attribute.value.has_value()) {
        return {};
    }

    const AST::AttributeValue &value = attribute.value.value();

    if (value.kind == AST::AttributeValueKind::t_record) {
        if (const AST::AttributeField *field = value.find_field(property.name)) {
            return value_key_text(field->value);
        }
        return {};
    }

    return value_key_text(value);
}

void AST::attr_bind_payload(AST::AttrRow &row, const AST::AttrSchema &schema)
{
    const AST::ComplexType *ct = schema.row_type.get_complex_type();
    if (ct == nullptr || row.match.attribute == nullptr || !row.match.attribute->value.has_value()) {
        return;
    }

    const AST::AttributeValue &value = row.match.attribute->value.value();

    for (size_t index : schema.payload_indexes) {
        const AST::ComplexType::Property &property = ct->get_property(index);
        const AST::AttributeValue *field_value = &value;
        if (value.kind == AST::AttributeValueKind::t_record) {
            const AST::AttributeField *named = value.find_field(property.name);
            field_value = named != nullptr ? &named->value : nullptr;
        }

        if (field_value == nullptr) {
            continue;
        }

        AST::AttrPayloadSlot slot;
        slot.index = property.index;
        slot.type = property.type;
        slot.value = field_value;
        row.payload.push_back(slot);
    }
}
