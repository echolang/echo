#ifndef ASTSOURCETOKEN_H
#define ASTSOURCETOKEN_H

#pragma once

#include "Token.h"

namespace AST
{
    class Node;
    class ExprNode;

    // **where did the author write this node.**
    //
    // one taxonomy behind every source position the compiler attaches to anything - a diagnostic's
    // caret, an `assert` message's `<file>:<line>`, a DILocation. location_of_expression is a
    // spelling over this, for expressions only
    //
    // **null is a real answer and not a failure.** A node the parser never built out of a token has no
    // position of its own - a synthesized release, an implicit cast, a scope - and a caller wanting one
    // falls back to whatever it was already talking about. What makes that safe is that the switch
    // behind this has **no default**: a node kind added later does not compile until it has said which
    // of the two it is, so a null here is always a decision rather than an omission.
    //
    // the shapes that carry no token the author wrote borrow their operand's, recursively - `&`, a
    // deref, an implicit cast, a retain, a release. That was already location_of_expression's rule.
    //
    // **borrowing from a child is a per-arm judgement, not a rule to follow by default.** An `if` and a
    // `while` take their condition's token because a condition sits on the keyword's line, and a plain
    // scope answers null because a brace has no equivalent to borrow from. So a new arm decides what
    // *its* kind means rather than reaching for the nearest precedent.
    //
    // **the file is on the token**, not on this answer. A DILocation still takes its file from the
    // scope it hangs off rather than from itself; a diagnostic reads `token.file()` through the slice
    const TokenReference *source_token_of(const Node &node);

    // the expression half, kept at its old name and signature for its four callers - all of which
    // report a diagnostic about an expression, where having no token at all is a compiler bug rather
    // than a case to handle. that is what its assert has always said; what changed is that the *arms*
    // moved, so a node kind added without one now fails to compile above rather than reaching a
    // default that returned a reinterpreted reference in release
    const TokenReference &location_of_expression(ExprNode *expr);
};

#endif
