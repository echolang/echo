#ifndef EXPRPARSER_H
#define EXPRPARSER_H

#pragma once

#include "Parser/ParserPayload.h"
#include "AST/ExprNode.h"
#include "AST/StaticPropertyExprNode.h"

#include <unordered_map>

namespace Parser
{
    AST::ExprNode *parse_expr(Payload &payload, AST::TypeNode *expected_type = nullptr);
    const AST::NodeReference parse_expr_ref(Payload &payload, AST::TypeNode *expected_type = nullptr);

    // parses a chain of `->member` accesses onto `base`, wrapping it in one
    // MemberAccessNode per level. returns `base` unchanged when there is no `->`.
    // on a malformed chain (missing identifier after `->`) it collects an
    // UnexpectedToken issue and returns make_void_ref(); callers do their own recovery
    const AST::NodeReference parse_postfix_chain(Payload &payload, AST::NodeReference base);

    // **`Type::f(...)`, a call whose candidates come from a type rather than a namespace.** null when
    // these tokens are not one, with the cursor put back exactly as it was found - so an operand
    // position tries this first and reads a namespace-qualified call as it always did.
    //
    // it has to be tried *before* Parser::parse_namespace, which mints what it does not find: once
    // that has consumed `Point::` there is a namespace called `Point` and the type is unreachable.
    // it also has to *commit* once the owner is known to be a type - falling back to the namespace
    // path there is what lets FunctionRegistry::overloads walk outward and answer `Foo::f()` with an
    // enclosing free `f`, which is the pre-existing bug this arm must not inherit
    AST::FunctionCallExprNode *try_parse_static_call(Payload &payload);

    // **does a `.name(` start here?** - the shorthand static call, whose owner comes from wherever its
    // value goes rather than from anything written at the call site.
    //
    // one predicate with two readers, which is the point: `is_expr_token` decides whether the
    // shunting-yard loop enters at all and `parse_operand` decides what to build, and a disagreement
    // between them is not a diagnostic - it is the compiler aborting on a sanity assert with no
    // location. guarded on the identifier *and* what follows it so `..`, which is two `t_dot` and a
    // declared infix operator, can never be claimed
    bool starts_shorthand_call(Cursor &cursor);

    // **`Type::$x`, a read or a write of storage the type owns.** null when these tokens are not
    // one, cursor untouched - and, like the call form above, tried before Parser::parse_namespace
    // for the same reason: once that has minted a namespace called `Point` the type is out of
    // reach, and a `$name` after the `::` matches no arm in parse_operand at all
    AST::StaticPropertyExprNode *try_parse_static_property(Payload &payload);

    // **does `[identifier [<...>] ::]+ $name` start here?** - the shape a static property access is
    // written in, measured without deciding that the prefix names a type. that second question is
    // try_parse_static_property's, asked once, inside the expression parse.
    //
    // one scanner because two readers have to agree about the same token run: the statement dispatch,
    // which owes `Session::$count = 1;` a branch it is otherwise anchored on nothing to reach, and the
    // operand parser that reads it. a disagreement between them is silent in the worst direction - the
    // statement falls to the catch-all and reports an unexpected identifier, saying nothing about statics
    bool starts_static_property(Cursor &cursor);

    // `weak($obj)` / `weak<Foo>($obj)`, and `strong($w)`. the two halves of a weak reference, written
    // rather than implicit because each moves a reference count and one of them can fail
    //
    // `weak(...)` builds the very same AddrOfExprNode a written `&` does, so the operation has one
    // lowering however it was spelled; `strong(...)` builds a StrongExprNode. both return nullptr after
    // collecting a located issue, which is the recovery convention every parser here follows
    AST::ExprNode *parse_weak_expr(Payload &payload);
    AST::ExprNode *parse_strong_expr(Payload &payload);

    // true when the cursor sits on `const(E)`, the value the compiler is required to work out.
    //
    // **gated on the `(`, and that is the whole content of the question**: a bare `const` begins a
    // declaration or a `const if`, and neither is an expression. one owner because two sites ask and they
    // must agree - the token that admits an expression into the shunting-yard loop, and the production
    // that reads one - and disagreement is silent: the loop never enters, the expression comes back empty
    // and parse_expr_ref's sanity assert takes the compiler down.
    //
    // the fourth member of the partition on a leading `const`, beside Parser::starts_const_if,
    // Parser::starts_constdecl and Parser::starts_vardecl. it is the only one that is an *expression*,
    // so it is asked where expressions are and not at the statement dispatch
    bool starts_const_expr(const Parser::Cursor &cursor);
};

#endif
