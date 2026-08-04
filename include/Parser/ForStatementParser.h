#ifndef FORSTATEMENTPARSER_H
#define FORSTATEMENTPARSER_H

#pragma once

#include "AST/ForStatementNode.h"
#include "AST/ScopeNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // `for (init; condition; step) { }`.
    //
    // **hands back the wrapper scope, not the loop.** the init is an ordinary statement in a scope of its
    // own holding the loop beside it, which is what makes `$i` visible to the condition, the step and the
    // body and dead after the loop - with no lifetime rule anywhere that a block does not already have.
    // see AST::ForStatementNode for why the init is not an edge on the node
    //
    // all three clauses are required. an omitted one has an obvious spelling that means something else
    // (`while` is the two-clause loop, and a `for` with no step is a `while` written the long way), so a
    // missing clause is a located error rather than a silently different loop
    AST::ScopeNode *parse_forstatement(Parser::Payload &payload);
};

#endif
