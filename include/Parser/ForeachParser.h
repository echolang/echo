#ifndef FOREACHPARSER_H
#define FOREACHPARSER_H

#pragma once

#include "AST/ForeachNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // ```
    // foreach ( <expr> as [const] [&] $el ) { ... }
    // foreach ( <expr> as $k => [const] [&] $el ) { ... }
    // ```
    //
    // modelled on Parser::parse_guard rather than on parse_whilestatement: this is a statement whose
    // *shape* contains declarations, and they have to be resolvable while the body that reads them is
    // parsed. null when it reported and gave up
    AST::ForeachNode *parse_foreach(Parser::Payload &payload);
};

#endif
