#ifndef IFSTATEMENTPARSER_H
#define IFSTATEMENTPARSER_H

#pragma once

#include "AST/ConstIfNode.h"
#include "AST/IfStatementNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    AST::IfStatementNode *parse_ifstatement(Parser::Payload &payload);

    // true when the cursor sits on a **compile-time** branch, `const if (...)`.
    //
    // two tokens rather than a type-grammar scan, which is why it lives here beside the branch parsers
    // instead of in TypeParser.h beside its two siblings. it is nonetheless the **third member of the
    // partition on a leading `const`**: `starts_constdecl` claims `const NAME = ...`, `starts_vardecl`
    // claims `const T $x`, and this claims `const if`. every dispatch site asks the other two in that
    // order, *and* starts_vardecl defers to this one - so the three answer yes to disjoint inputs rather
    // than relying on the dispatch order alone
    bool starts_const_if(const Parser::Cursor &cursor);

    AST::ConstIfNode *parse_const_ifstatement(Parser::Payload &payload);
};

#endif
