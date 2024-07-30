#ifndef NAMESPACEPARSER_H
#define NAMESPACEPARSER_H

#pragma once

#include "Parser/ParserPayload.h"
#include "AST/NamespaceNode.h"
#include "AST/NamespaceDeclNode.h"

namespace Parser
{
    AST::NamespaceNode *parse_namespace(Payload &payload);
    AST::NamespaceDeclNode *parse_namespacedecl(Payload &payload);
};



#endif