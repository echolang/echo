#ifndef EXPRPARSER_H
#define EXPRPARSER_H

#pragma once

#include "ParserPayload.h"
#include "AST/ExprNode.h"

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