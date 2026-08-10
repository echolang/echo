#include "AST/ASTAttributeValue.h"

#include <fmt/core.h>

const char *AST::attribute_value_noun(AST::AttributeValueKind kind)
{
    switch (kind) {
    case AST::AttributeValueKind::t_string: return "a string";
    case AST::AttributeValueKind::t_int: return "a number";
    case AST::AttributeValueKind::t_float: return "a number";
    case AST::AttributeValueKind::t_bool: return "true or false";
    case AST::AttributeValueKind::t_name: return "a name";
    case AST::AttributeValueKind::t_list: return "a list";
    case AST::AttributeValueKind::t_record: return "a record";
    }

    return "a value";
}

const std::string &AST::AttributeValue::tag() const
{
    static const std::string none;

    if (!tag_span.is_valid()) {
        return none;
    }

    return tag_span.first().value();
}

const AST::AttributeField *AST::AttributeValue::find_field(const std::string &key) const
{
    for (const AST::AttributeField &field : fields) {
        if (field.key == key) {
            return &field;
        }
    }

    return nullptr;
}

std::string AST::attribute_value_spelling(const AST::AttributeValue &value)
{
    std::string spelled;

    if (value.has_tag()) {
        spelled += value.tag() + " ";
    }

    switch (value.kind) {
    case AST::AttributeValueKind::t_string:
        // re-quoted rather than re-escaped: this is read by a message quoting a value back, and the
        // excerpt beside it already shows the source byte for byte
        spelled += "\"" + value.text + "\"";
        break;

    case AST::AttributeValueKind::t_int:
        spelled += std::to_string(value.integer);
        break;

    case AST::AttributeValueKind::t_float:
        spelled += fmt::format("{}", value.number);
        break;

    case AST::AttributeValueKind::t_bool:
        spelled += value.boolean ? "true" : "false";
        break;

    case AST::AttributeValueKind::t_name:
        spelled += value.text;
        break;

    case AST::AttributeValueKind::t_list: {
        spelled += "[";
        for (size_t i = 0; i < value.items.size(); i++) {
            spelled += (i > 0 ? ", " : "") + AST::attribute_value_spelling(value.items[i]);
        }
        spelled += "]";
        break;
    }

    case AST::AttributeValueKind::t_record: {
        spelled += "{";
        for (size_t i = 0; i < value.fields.size(); i++) {
            spelled += (i > 0 ? ", " : " ") + value.fields[i].key + ": "
                + AST::attribute_value_spelling(value.fields[i].value);
        }
        spelled += value.fields.empty() ? "}" : " }";
        break;
    }
    }

    return spelled;
}
