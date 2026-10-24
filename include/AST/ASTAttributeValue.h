#ifndef ASTATTRIBUTEVALUE_H
#define ASTATTRIBUTEVALUE_H

#pragma once

#include "Token.h"

#include <cstdint>
#include <string>
#include <vector>

namespace AST
{
    // what an attribute value is. a bare name means itself, never a constant or a type.
    // optionally tagged (`framework "OpenGL"`). not an expression node: it never reaches codegen
    enum class AttributeValueKind
    {
        t_string,   // "..."       - free text, quotes stripped and escapes decoded
        t_int,      // 42, 0x2a    - and a negated one
        t_float,    // 1.5
        t_bool,     // true, false
        t_name,     // array       - a bare identifier, meaning itself
        t_list,     // [a, b]
        t_record,   // { k: v }
    };

    // the noun a refusal calls this kind - "a string", "a name", "a record". one owner, so a message
    // about the wrong shape reads the same wherever it is written
    const char *attribute_value_noun(AttributeValueKind kind);

    struct AttributeField;

    struct AttributeValue
    {
        AttributeValueKind kind = AttributeValueKind::t_name;

        // the whole value, tag included - what a refusal about the value as a whole underlines
        TokenSpan span;

        // the tag's own token, or an invalid span when untagged. a *separate* span rather than a flag
        // on the value, because "'framwork' is not a link scheme" has to underline the tag and not the
        // payload sitting beside it
        TokenSpan tag_span;

        // the tag's decoded text, or empty when there is none. stored rather than re-read off
        // tag_span: a string tag's token still holds its quotes, and tag() has to answer the
        // decoded name - `"libcurl" { ... }` is the package libcurl
        std::string tag_text;

        std::string text;                     // t_string, decoded - or t_name, verbatim
        int64_t integer = 0;                  // t_int
        double number = 0.0;                  // t_float
        bool boolean = false;                 // t_bool
        std::vector<AttributeValue> items;    // t_list
        std::vector<AttributeField> fields;   // t_record, in the order they were written

        bool is(AttributeValueKind of_kind) const {
            return kind == of_kind;
        }

        bool has_tag() const {
            return tag_span.is_valid();
        }

        // the tag's text, or an empty string when there is none
        const std::string &tag() const;

        // the field written under `key`, or null. linear over `fields` on purpose: a record holds a
        // handful of keys and keeping them in written order is what makes a diagnostic point at the
        // first offender rather than at whichever one a hash happened to visit
        const AttributeField *find_field(const std::string &key) const;
    };

    struct AttributeField
    {
        TokenSpan key_span;
        std::string key;
        AttributeValue value;
    };

    // `lib "GL"`, `{ a: 1, b: "x" }` - **the one place a *written* value is rendered back**, read by
    // AttributeNode::node_description so `-a` shows what was written and by every refusal that quotes a
    // value it still holds.
    //
    // Deliberately not what Compiler::link_requirement_spelling renders: that one quotes a requirement
    // *after* it settled, whose value is an absolute path the manifest never contained, so it has no
    // AttributeValue left to hand here
    std::string attribute_value_spelling(const AttributeValue &value);
};

#endif
