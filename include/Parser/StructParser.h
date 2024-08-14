#ifndef STRUCTPARSER_H
#define STRUCTPARSER_H

#pragma once

#include "AST/StructNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    AST::StructDeclNode *parse_struct(Payload &payload, bool symbol_only = false);
};


#endif