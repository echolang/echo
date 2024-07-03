#ifndef WHILESTATEMENTPARSER_H
#define WHILESTATEMENTPARSER_H

#pragma once

#include "AST/WhileStatementNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    AST::WhileStatementNode *parse_whilestatement(Parser::Payload &payload);
};

#endif