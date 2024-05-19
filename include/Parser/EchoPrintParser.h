#ifndef ECHOPRINTPARSER_H
#define ECHOPRINTPARSER_H

#pragma once

#include "ParserPayload.h"

namespace AST
{
    class FunctionCallExprNode;
};

namespace Parser
{
    AST::FunctionCallExprNode * parse_echo(Payload &payload);
};


#endif