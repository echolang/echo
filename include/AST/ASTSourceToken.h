#ifndef ASTSOURCETOKEN_H
#define ASTSOURCETOKEN_H

#pragma once

#include "Token.h"

#include <cstdint>
#include <string>

namespace AST
{
    class Node;
    class ExprNode;
    class File;

    // the author's token for this node, or null when there is none (a synthesized release, a scope).
    // the switch has no default. location_of_expression asserts for expressions
    const TokenReference *source_token_of(const Node &node);

    const TokenReference &location_of_expression(ExprNode *expr);

    // **what source site this file is.** path relative to the module directory, never a leaf
    // when the module has directories and never an absolute checkout path. coverage, abort
    // records and `std::src::here()` all read this so they cannot drift
    std::string source_file_of(const File &file);

    // one source site: the token's file/line/column plus the module that owns the file
    struct SourceSite
    {
        std::string module;
        std::string file;
        uint32_t line = 0;
        uint32_t column = 0;
    };

    // empty file when the node has no token. minted tokens keep the file and answer 0 for line
    SourceSite source_site_of(const Node &node);
};

#endif
