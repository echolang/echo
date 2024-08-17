#ifndef TYPEPARSER_H
#define TYPEPARSER_H

#pragma once

#include "AST/ASTContext.h"
#include "AST/TypeNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    bool can_parse_type(Payload &payload);

    AST::TypeNode *parse_type(Payload &payload);

    // Parses an optional generic type-parameter list `<T, U, ...>` (the declaration side,
    // e.g. on a function or struct). Returns the parameter names, or an empty vector if the
    // cursor is not positioned at a `<`. Consumes through the closing `>`.
    std::vector<std::string> parse_type_param_list(Payload &payload);
};


#endif