#ifndef MATCHPARSER_H
#define MATCHPARSER_H

#pragma once

#include "AST/MatchExprNode.h"
#include "Parser/ParserPayload.h"

namespace AST
{
    class TypeNode;
}

namespace Parser
{
    // `match ($unit) { ... }`. a keyword, so this is one token and the position it is asked in decides
    // nothing - a match is an operand wherever an operand may be, and a statement for the reason any
    // expression may be one
    inline bool starts_match(const Cursor &cursor) {
        return cursor.is_type(Token::Type::t_match);
    }

    // reads the whole form, cursor on `match`, and consumes through its closing brace. null when it did
    // not parse, having reported and recovered.
    //
    // **the patterns are not resolved here.** `.timeout($s)` says which enum it means only by where it
    // is, and even `Unit::meter` names a case whose payload types the subject's own type has to agree
    // with - so what this builds is the shape, and AST::MatchResolution fills in which case each arm
    // selects and what each binding holds once the fixpoint has settled the subject
    // `expected_type` is the destination the whole form is going to, when the position
    // already knows one: a typed declaration, a return. value arms are parsed against it
    // so `.ok($v)` and `.normal` bind the way they do outside a match. null when the
    // match is a statement, or an operand whose destination is not known yet
    AST::MatchExprNode *parse_match(Payload &payload, AST::TypeNode *expected_type = nullptr);
};

#endif
