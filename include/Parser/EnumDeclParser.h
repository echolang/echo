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
    // `collect_members` gates only what is kept. what is kept is the EnumCase and, for a payload case,
    // its constructor. a payload-free constructor waits for finalize_enum: a leftover case must not
    // become a static, and parse_enum_case cannot yet know it is looking at one
    void parse_enum_case(
        Payload &payload,
        AST::TypeDeclNode *enum_node,
        const AST::ValueType &self_value_type,
        bool collect_members);

    // **after the case list is complete.** classifies the leftover, mints a constructor for every
    // payload-free case that is not one, then synthesizes `value()` and `from()`. that order is
    // load-bearing: `from` calls the case constructors, and a remainder has no constructor to call
    void finalize_enum(
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
