#ifndef ATTRIBUTEVALUEPARSER_H
#define ATTRIBUTEVALUEPARSER_H

#pragma once

#include "AST/ASTAttributeValue.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // **the whole of what may sit after the colon in an attribute:**
    //
    //     value   := name atom          // tagged - `framework "OpenGL"`, `git { ... }`
    //              | atom
    //     atom    := string | int | float | bool | name | list | record
    //     list    := '[' ( value ( ',' value )* ','? )? ']'
    //     record  := '{' ( name ':' value ( ',' name ':' value )* ','? )? '}'
    //
    // a closed data grammar, and deliberately *not* Parser::parse_expr_ref - see AST::AttributeValue
    // for the four shapes that one cannot say and the namespace it mints on the way. Nothing here
    // touches the collector's core types, the type registry or a namespace, because a `module.eco` is
    // read into a scratch bundle that has none of them.
    //
    // reports **shape** only. Whether `framework` is a link scheme, or whether a record was allowed to
    // carry a `rev`, is the consumer's question and AST::AttributeReader's to refuse
    bool parse_attribute_value(Parser::Payload &payload, AST::AttributeValue &out);

    // does a value start at `offset` tokens from the cursor?
    //
    // **the one token of lookahead that separates `name` from `name atom`**, and therefore the one
    // owner of where a tag ends: after `#[core: array]` the next token is `]`, which starts nothing, so
    // `array` is a name rather than a tag with no payload
    bool starts_attribute_value(const Parser::Cursor &cursor, size_t offset = 0);

    // expects the `:` that separates an attribute from its value, or a record key from its.
    //
    // **the one owner of the `:$` trap**: `:$` lexes as the pointer-of operator, which is the single
    // place in the grammar where a colon glued to a `$` means something else, and it is now reachable
    // from a record field (`{ path:$x }`) as well as from the attribute head. one sentence, said once
    bool expect_attribute_colon(Parser::Payload &payload);
};

#endif
