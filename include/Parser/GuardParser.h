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
    // `T $x = guard <nullable> else { ... }` - see AST::GuardNode for what the form means
    //
    // **`guard` is an initializer form, not a statement head**, so the declaration is not this
    // function's to read: Parser::parse_varexpr owns the type, the name, the `=` and the registration,
    // and hands the binding in. what is left is everything from the `guard` keyword to the else arm's
    // closing brace, which is where the cursor sits on entry and just past on return.
    //
    // the binding is registered by **name only** (AST::ScopeNode::declare_variable), because its
    // initializer runs once inside the branch that found a value rather than as a statement of the
    // enclosing scope - and it is registered into that *enclosing* scope, so from the guard onwards
    // `$x` is an ordinary non-null local. that is the whole point of the form.
    //
    // `is_const` is the declaration's, and it is passed rather than re-read because the binding's type
    // is inferred from the *payload* here and AST::infer_declaration_type wants both halves at once
    AST::GuardNode *parse_guard(Payload &payload, AST::VarDeclNode &binding, bool is_const);
};

#endif
