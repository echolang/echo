#include "AST/ASTAttributeReader.h"

namespace
{
    std::vector<const AST::AttributeValue *> peel(const AST::AttributeValue &value, bool peel_it)
    {
        std::vector<const AST::AttributeValue *> values;

        if (!peel_it) {
            values.push_back(&value);
            return values;
        }

        for (const AST::AttributeValue &item : value.items) {
            values.push_back(&item);
        }

        return values;
    }
};

std::vector<const AST::AttributeValue *> AST::AttributeReader::each(const AST::AttributeValue &value)
{
    // an untagged list *is* the list; a tagged one is a value that happens to be a list, and belongs
    // to whoever declared the tag. so the tag is what stops the peel
    return peel(value, value.is(AST::AttributeValueKind::t_list) && !value.has_tag());
}

std::vector<const AST::AttributeValue *> AST::AttributeReader::payload(const AST::AttributeValue &value)
{
    return peel(value, value.is(AST::AttributeValueKind::t_list));
}

void AST::AttributeReader::refuse(const TokenSpan &span, const std::string &message)
{
    _refusals.push_back(AST::AttributeRefusal { message, span });
}

void AST::AttributeReader::wrong_shape(const AST::AttributeValue &value, AST::AttributeValueKind wanted)
{
    refuse(value.span, fmt::format(
        "the '{}' attribute wants {} here, not {}.",
        _attribute_name, AST::attribute_value_noun(wanted), AST::attribute_value_noun(value.kind)));
}

std::optional<std::string> AST::AttributeReader::string(const AST::AttributeValue &value)
{
    if (!value.is(AST::AttributeValueKind::t_string)) {
        wrong_shape(value, AST::AttributeValueKind::t_string);
        return std::nullopt;
    }

    return value.text;
}

std::optional<std::string> AST::AttributeReader::name(const AST::AttributeValue &value)
{
    if (!value.is(AST::AttributeValueKind::t_name)) {
        wrong_shape(value, AST::AttributeValueKind::t_name);
        return std::nullopt;
    }

    return value.text;
}

std::optional<int64_t> AST::AttributeReader::integer(const AST::AttributeValue &value)
{
    if (!value.is(AST::AttributeValueKind::t_int)) {
        wrong_shape(value, AST::AttributeValueKind::t_int);
        return std::nullopt;
    }

    return value.integer;
}

std::optional<std::string> AST::AttributeReader::word(const AST::AttributeValue &value)
{
    switch (value.kind) {
    case AST::AttributeValueKind::t_string:
    case AST::AttributeValueKind::t_name:
        return value.text;

    case AST::AttributeValueKind::t_int:
        return std::to_string(value.integer);

    default:
        break;
    }

    refuse(value.span, fmt::format(
        "the '{}' attribute wants a name, a string or a number here, not {}.",
        _attribute_name, AST::attribute_value_noun(value.kind)));
    return std::nullopt;
}

bool AST::AttributeReader::record(const AST::AttributeValue &value)
{
    if (!value.is(AST::AttributeValueKind::t_record)) {
        wrong_shape(value, AST::AttributeValueKind::t_record);
        return false;
    }

    return true;
}

const AST::AttributeValue *AST::AttributeReader::field(const AST::AttributeValue &value, const std::string &key)
{
    const AST::AttributeField *found = value.find_field(key);
    return found == nullptr ? nullptr : &found->value;
}

const AST::AttributeValue *AST::AttributeReader::require_field(const AST::AttributeValue &value, const std::string &key)
{
    const AST::AttributeValue *found = field(value, key);

    if (found == nullptr) {
        refuse(value.span, fmt::format(
            "this '{}' is missing its '{}'.", _attribute_name, key));
    }

    return found;
}

void AST::AttributeReader::reject_unknown_fields(const AST::AttributeValue &value, const std::vector<std::string> &known)
{
    for (const AST::AttributeField &field : value.fields) {
        bool is_known = false;

        for (const std::string &candidate : known) {
            if (candidate == field.key) {
                is_known = true;
                break;
            }
        }

        if (is_known) {
            continue;
        }

        std::string list;
        for (const std::string &candidate : known) {
            list += list.empty() ? candidate : ", " + candidate;
        }

        refuse(field.key_span, fmt::format(
            "'{}' is not something a '{}' knows, expected one of: {}.",
            field.key, _attribute_name, list));
    }
}
