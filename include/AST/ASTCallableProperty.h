#ifndef ASTCALLABLEPROPERTY_H
#define ASTCALLABLEPROPERTY_H

#pragma once

namespace AST
{
    class Bundle;

    // **`$a[0]->fn(1)` is a pending member call at parse time**: the index has no type until
    // OperatorRewriter attaches `operator []`. once the receiver is a struct with a callable
    // property of that name, the call is an IndirectCallExprNode - the same node the parser
    // plants when the receiver is already typed (`$b->fn(1)` on a `const Box&`).
    //
    // inside the monomorphizer's settle(), ahead of settle_calls, so the rewritten node is
    // what that round resolves. AST::callable_property_of is the question; this is the retry
    bool rewrite_callable_property_calls(Bundle &bundle);
};

#endif
