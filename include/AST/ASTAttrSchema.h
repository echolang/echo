#ifndef ASTATTRSCHEMA_H
#define ASTATTRSCHEMA_H

#pragma once

#include "AST/ASTAttrCollect.h"

#include <optional>
#include <string>
#include <string_view>

namespace AST
{
    class CoreTypes;
    class AttributeNode;

    struct AttrSliceLayout
    {
        const ComplexType::Property *data = nullptr;
        const ComplexType::Property *len = nullptr;
    };

    // structural slice: `$data` a pointer and `$len` an integer. codegen throws when a
    // list intern accepted still has no layout
    std::optional<AttrSliceLayout> attr_slice_layout(const ValueType &field);

    std::optional<std::string> attr_payload_shape_refusal(
        const AttrSchema &schema,
        const AttributeNode &attribute,
        const CoreTypes &core
    );

    std::optional<std::string> attr_row_refusal(const AttrMatch &hit, const AttrSchema &schema);

    std::optional<std::string> attr_key_field_refusal(
        const AttrSchema &schema,
        std::string_view key_field,
        const CoreTypes &core
    );

    std::string attr_payload_field_text(
        const AttributeNode &attribute,
        const ComplexType::Property &property
    );

    // intern already typed each payload field; codegen lowers `row.payload`
    void attr_bind_payload(AttrRow &row, const AttrSchema &schema);
};

#endif
