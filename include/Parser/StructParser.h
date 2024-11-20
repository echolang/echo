#ifndef STRUCTPARSER_H
#define STRUCTPARSER_H

#pragma once

#include "AST/StructNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // a struct body is walked in *both* the declaration and the body pass, by the same code, so the
    // two cannot disagree about where a member ends - they differ only in what they keep, which
    // `payload.pass` tells them
    AST::StructDeclNode *parse_struct(Payload &payload);
};


#endif
