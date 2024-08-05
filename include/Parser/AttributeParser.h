#ifndef ATTRIBUTEPARSER_H
#define ATTRIBUTEPARSER_H

#pragma once

#include "ParserPayload.h"
#include "AST/AttributeNode.h"

namespace Parser
{
    AST::AttributeNode *parse_attribute(Parser::Payload &payload);
};

#endif