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

    // **is this `as` foreach's binding?** `as $el`, `as &$el`, `as const &$el`, and the refused
    // `as const $el`. `as const int32` is a cast - the token after `const` is a type, not a
    // binding. cursor sits on the `as`.
    //
    // parse_postfix_chain is the reader: it has to leave the keyword in the stream or
    // parse_foreach never sees it. one owner so the peek is not recopied the next time
    // something else reads `as`
    bool starts_foreach_as_binding(const Cursor &cursor);
};

#endif
