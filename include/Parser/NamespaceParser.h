#ifndef NAMESPACEPARSER_H
#define NAMESPACEPARSER_H

#pragma once

#include "Parser/ParserPayload.h"
#include "AST/NamespaceNode.h"
#include "AST/NamespaceDeclNode.h"

namespace Parser
{
    // the offset just past the `identifier ::` pairs starting at `offset`, so a lookahead can
    // step over a namespace qualification and ask its real question about what follows. every
    // statement dispatch that has to see through `a::b::` uses this rather than walking the
    // pairs itself. pure lookahead, the cursor is not moved
    size_t peek_past_namespace_prefix(Payload &payload, size_t offset = 0);

    AST::NamespaceNode *parse_namespace(Payload &payload);
    AST::NamespaceDeclNode *parse_namespacedecl(Payload &payload);
};



#endif