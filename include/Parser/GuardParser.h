#ifndef GUARDPARSER_H
#define GUARDPARSER_H

#pragma once

#include "ParserPayload.h"

namespace AST
{
    class GuardNode;
    class ScopeNode;
};

namespace Parser
{
    // `guard T $x = <nullable> else { ... }` - see AST::GuardNode for what the form means
    //
    // `scope` is the **enclosing** scope, and the binding is registered into it rather than into the else
    // arm: from the guard onwards `$x` is an ordinary non-null local, which is the whole point of the
    // statement. a caller passing null gets a well-formed node whose declaration nothing can resolve,
    // which is what every other declaration parser does with the same argument
    AST::GuardNode *parse_guard(Payload &payload, AST::ScopeNode *scope);
};

#endif
