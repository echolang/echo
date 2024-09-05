#ifndef EXTERNPARSER_H
#define EXTERNPARSER_H

#pragma once

#include "AST/FunctionDeclNode.h"
#include "Parser/ParserPayload.h"

#include <vector>

namespace Parser
{
    // parses an `extern { ... }` block of bodyless C function declarations:
    //
    //   extern {
    //       function malloc as alloc_bytes(usize $bytes) : ptr<uint8>;
    //       function free(ptr<uint8> $p) : void;
    //   }
    //
    // returns every declaration it read, so the symbol pass can register them in the current
    // namespace. `symbol_only` mirrors parse_funcdecl's flag - the block is walked in both parser
    // passes, which is exactly what keeps the extern-ness on both nodes for a declaration
    std::vector<AST::FunctionDeclNode *> parse_extern_block(Parser::Payload &payload, bool symbol_only = false);
};

#endif
