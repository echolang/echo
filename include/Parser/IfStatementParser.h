#ifndef IFSTATEMENTPARSER_H
#define IFSTATEMENTPARSER_H

#pragma once

#include "AST/IfStatementNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    AST::IfStatementNode *parse_ifstatement(Parser::Payload &payload);
};

#endif