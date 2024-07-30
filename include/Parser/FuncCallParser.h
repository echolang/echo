#ifndef FUNCCALLPARSER_H
#define FUNCCALLPARSER_H

#pragma once

#include "AST/ExprNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    AST::FunctionCallExprNode *parse_funccall(Parser::Payload &payload, const AST::Namespace *requested_namespace = nullptr);
};

#endif