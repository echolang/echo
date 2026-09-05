#ifndef OPAQUEDECLPARSER_H
#define OPAQUEDECLPARSER_H

#pragma once

#include "AST/TypeDeclNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // `extern struct Name;` and `struct Name;` inside an `extern { }` block. one function so the
    // standalone form and the block form cannot disagree about kind, semicolon, or what is refused.
    // `visibility` is the modifier written ahead of the standalone declaration or ahead of the
    // block, already resolved through AST::declaration_visibility
    AST::TypeDeclNode *parse_opaque_typedecl(Payload &payload, AST::Visibility visibility);
};

#endif
