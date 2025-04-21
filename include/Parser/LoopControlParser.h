#ifndef LOOPCONTROLPARSER_H
#define LOOPCONTROLPARSER_H

#pragma once

#include "AST/LoopControlNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // `break;` and `continue;`. hands back null - and builds no node at all - when there is no loop to
    // leave, which is the whole of its validation
    AST::LoopControlNode *parse_loop_control(Parser::Payload &payload);
};

#endif
