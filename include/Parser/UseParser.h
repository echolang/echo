#ifndef USEPARSER_H
#define USEPARSER_H

#pragma once

#include "Parser/ParserPayload.h"

namespace AST
{
    class UseDeclNode;
};

namespace Parser
{
    // a file-scope `use` statement. records bindings on the File in the type-name pass and
    // plants a UseDeclNode on the file root in the body pass. `at_file_scope` is the caller's
    // reading of "not inside a body or a type" - pass 1 has no lexical namespace, so it has to
    // say so itself
    AST::UseDeclNode *parse_usedecl(Payload &payload, bool at_file_scope);
};

#endif
