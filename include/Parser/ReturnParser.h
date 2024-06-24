#ifndef RETURNPARSER_H
#define RETURNPARSER_H

#pragma once

#include "AST/ReturnNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    AST::ReturnNode &parse_return(Parser::Payload &payload);
};

#endif