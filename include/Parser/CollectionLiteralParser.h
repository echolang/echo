#ifndef COLLECTIONLITERALPARSER_H
#define COLLECTIONLITERALPARSER_H

#pragma once

#include "Parser/ParserPayload.h"

#include <vector>

namespace AST
{
    class ExprNode;
};

namespace Parser
{
    // `[1, 2, 3]` / `["k" => v]`. cursor on `[`. mints the node and consumes through `]`.
    // null when it reported and gave up. empty `[]` is an array literal: a map destination
    // fills it through the constructor-only path
    AST::ExprNode *parse_collection_literal(Payload &payload);

    // the comma-separated expressions between a `[` and its `]`, cursor already past the
    // opening bracket and left after the closing one. answers false having reported when
    // the list does not close. one grammar with the collection literal, because an index
    // list (`$m[$r, $c]`) asks the same loop. emptiness is the caller's question
    bool parse_bracketed_expr_list(Payload &payload, std::vector<AST::ExprNode *> &elements);
};

#endif
