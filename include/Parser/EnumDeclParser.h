#ifndef ENUMDECLPARSER_H
#define ENUMDECLPARSER_H

#pragma once

#include "AST/TypeDeclNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // `case meter;` - the one member shape an enum has that no other kind does. a real keyword rather
    // than a contextual word, unlike `constructor` beside it: `case timeout(int32 $a);` and a statement
    // calling a function named `timeout` are the same tokens, and only the keyword tells them apart
    inline bool starts_enum_case(const Cursor &cursor) {
        return cursor.is_type(Token::Type::t_case);
    }

    // **appends `__tag`, and must be called before the first case is read** - the discriminant is
    // property 0 (AST::k_enum_tag_index), so it has to be appended before anything else is.
    //
    // its type is the backing type for an integer-backed enum, where the discriminant *is* the written
    // value, and `uint8` otherwise. one fixed width rather than one chosen from the case count, because
    // a width settled after the body was read would have to be patched into a property the body already
    // referred to - and because the escape is a spelling the author already has: an enum of more than
    // 256 cases is refused here, naming `: int32` as the answer
    void declare_enum_tag(Payload &payload, AST::TypeDeclNode *enum_node);

    // reads one case, cursor on the `case` keyword, and consumes through its `;`.
    //
    // **parsed in both passes and recorded in one**, which is the rule the whole type body follows: the
    // two walks must agree about where a member ends, so the consuming half is unconditional and
    // `collect_members` gates only what is kept. what is kept is three things that are one declaration -
    // the AST::ComplexType::EnumCase, the payload's properties, and the static function that builds it
    void parse_enum_case(
        Payload &payload,
        AST::TypeDeclNode *enum_node,
        const AST::ValueType &self_value_type,
        bool collect_members);

    // **`$unit->value()` for a backed enum**, and nothing at all for one without a backing.
    //
    // a *function* rather than a property, and uniformly for both backings, which is the one place this
    // deviates from CONCEPT.md's spelling. an integer backing could have been a property - the
    // discriminant already is the value - but a `string` one cannot: a string per enum value would mean
    // every `Unit` carried a refcounted handle, and `Unit::meter` would allocate. so what is stored is
    // always the tag, and this is what recovers the spelling.
    //
    // its body is a `match` over `$this`, which is the feature reading itself: `case meter = "m"` and
    // `Unit::meter => "m"` are the same statement said twice, and writing the second out of the first is
    // what keeps the accessor honest when a case is added
    void synthesize_backing_accessor(
        Payload &payload,
        AST::TypeDeclNode *enum_node,
        const AST::ValueType &self_value_type);

    // may this type back an enum, and why not. the vocabulary is closed - the integer primitives and
    // `string` - so a refusal can name what is allowed rather than describing a shape.
    //
    // empty when there is nothing to refuse, the wording rule AST::guard_payload_refusal follows, so a
    // caller asks once instead of asking a predicate and then building a sentence.
    //
    // takes the core types rather than comparing a spelling, `string` being a name the standard library
    // binds and not one the compiler knows - so under `--no-stdlib` an integer backing still works and
    // a `: string` is refused with this sentence rather than crashing on an unbound core type
    std::string enum_backing_refusal(const AST::ValueType &backing, const AST::CoreTypes &core);
};

#endif
