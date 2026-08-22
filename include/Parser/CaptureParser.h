#ifndef CAPTUREPARSER_H
#define CAPTUREPARSER_H

#pragma once

#include "AST/ExprNode.h"
#include "Parser/ParserPayload.h"

#include <vector>

namespace AST
{
    class ComplexType;
    class VarDeclNode;
};

namespace Parser
{
    // captures `vardecl` into the closure currently being parsed and answers the expression its body
    // should read instead: a member access on the environment parameter.
    //
    // capture is *by value* - the place is read once, here, in the frame the closure is created in - so a
    // closure is always safe to outlive that frame. the returned read is of the environment's copy, which
    // is why a later write to the original is not observed. a written list is closed: `$name` copies,
    // `mv $name` wraps the place in a MoveExprNode so the enclosing local is taken, and an unlisted
    // read is refused. no list is implicit copy of whatever the body reads
    //
    // more than one frame out walks `Context::closure_nest` from the current slot toward the front: the
    // outer closure captures the original, this one captures the outer environment's property. a named
    // `function` between the two is a nullptr slot, so that walk stops and the capture is refused
    //
    // null when the capture is refused, with the reason already reported
    AST::ExprNode *capture_variable(
        Payload &payload,
        AST::VarDeclNode *vardecl,
        const TokenReference &at,
        size_t boundaries_crossed
    );

    // `function[mv $a, $b]()` / `function[]()`. the cursor is on the `[`. the list is closed:
    // `$name` copies, `mv $name` moves, an empty list captures nothing. unknown names are refused
    // here rather than as unused after the body, because the list is written at the creation site
    // where those names have to be in scope already
    bool parse_capture_list(
        Payload &payload,
        std::vector<AST::ClosureExprNode::Capture> &captures
    );

    // a listed name the body never read. the list is closed, so naming one that is not used is
    // the error, not a silent extra capture
    void report_unused_captures(
        Payload &payload,
        const AST::ClosureExprNode &closure,
        const AST::ComplexType &environment
    );
};

#endif
