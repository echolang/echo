#ifndef TYPEALIASPARSER_H
#define TYPEALIASPARSER_H

#pragma once

#include "AST/TypeAliasDeclNode.h"
#include "Parser/ParserCursor.h"
#include "Parser/ParserPayload.h"
#include "Parser/VisibilityParser.h"

#include <optional>

namespace Parser
{
    // `type Name = ...;` or `type Name<T, ...> = ...;`. contextual: `type` is an identifier, so the
    // shape after it decides, and an interface's `type Iter : ...;` is not this
    bool starts_type_alias(const Cursor &cursor);

    // pass 1: mint the node, declare its parameters' arity, publish the name and skip to the `;`.
    // a declaration inside a block or a type body mints nothing and is refused by the body pass
    void declare_type_alias(Payload &payload, std::optional<AST::Visibility> visibility);

    // the declaration pass reads nothing of it: the step before it resolved the target already
    void skip_type_alias(Payload &payload);

    // the step between the first two passes. resolves every alias of this file whose target names
    // no unresolved alias, and answers whether any were
    bool resolve_file_type_aliases(Payload &payload);

    // after that step has stopped making progress, what is left names itself
    void mark_cyclic_type_aliases(const AST::File &file);

    // the body pass: the target again, in its own context with constraints known, the checks only the
    // declaration can make, and the node planted in the file root
    void parse_type_alias(Payload &payload, const VisibilityPrefix &visibility, bool in_block);
};

#endif
