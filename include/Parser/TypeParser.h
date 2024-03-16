#ifndef TYPEPARSER_H
#define TYPEPARSER_H

#pragma once

#include "AST/ASTContext.h"
#include "AST/TypeNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    bool can_parse_type(Payload &payload);

    AST::TypeNode &parse_type(Payload &payload);
};


#endif