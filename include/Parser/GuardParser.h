#ifndef GUARDPARSER_H
#define GUARDPARSER_H

#pragma once

#include "Parser/ParserPayload.h"

namespace AST
{
    class GuardNode;
    class VarDeclNode;
};

namespace Parser
{
    // from `guard` through the else arm. parse_varexpr hands the binding in; parse_scope passes null.
    // the binding is registered by name into the enclosing scope
    AST::GuardNode *parse_guard(Payload &payload, AST::VarDeclNode *binding, bool is_const);

    // `try <expr>;` - the guard whose else is `return .error($__fail)`. same three positions
    // as parse_guard: a declaration, an inferred declaration, or a statement
    AST::GuardNode *parse_try(Payload &payload, AST::VarDeclNode *binding, bool is_const);
};

#endif
