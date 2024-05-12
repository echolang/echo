#ifndef EXPRPARSER_H
#define EXPRPARSER_H

#pragma once

#include "ParserPayload.h"
#include "AST/ExprNode.h"

namespace Parser
{
    AST::ExprNode &parse_expr(Payload &payload);
};

#endif