#ifndef FUNCCALLPARSER_H
#define FUNCCALLPARSER_H

#pragma once

#include "AST/ASTCallResolution.h"
#include "AST/ASTOps.h"
#include "AST/ExprNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // a call written by name: `foo(...)`, `a::b::foo(...)`, `foo<int32>(...)`. The cursor sits on the name;
    // a namespace prefix has already been consumed by the caller and arrives as `requested_namespace`.
    //
    // `out_is_call` makes the explicit type argument list **speculative**, which is what an operand position
    // needs: a compile-time constant is a bare identifier, so `LIMIT < $n` reaches here on a `<` that opens
    // no type argument list. When it is given and no `(` follows the list, the cursor is restored to the
    // name, nothing is reported, and the caller reads the tokens as whatever else they are. Without it the
    // `<` is committed to and a missing `>` is an error - which is right at a statement head, where nothing
    // else could have been meant
    AST::FunctionCallExprNode *parse_funccall(
        Parser::Payload &payload,
        const AST::Namespace *requested_namespace = nullptr,
        bool *out_is_call = nullptr);

    // the call a user operator lowers to: an ordinary FunctionCallExprNode over the root namespace's
    // overload set for `op` at `fixity`, with the operands as its arguments. `at` is the operator's
    // symbol token, which is where the name is positioned and where a diagnostic points
    //
    // takes the fixity rather than the decorated name so a use site names its position **once**: the
    // three of them gate on a fixity and then have to spell the same one again to build the name, and
    // the name is the overload set's key - so a site that gated on suffix and named the prefix set
    // would compile and resolve against the wrong declarations
    //
    // from here on it is a call like any other - the fixpoint settles it, CallResolver coerces its
    // arguments, OwnershipPass copies what needs copying, and codegen emits a CreateCall. that is
    // why operator overloading needs no arm anywhere downstream
    //
    // **an unresolved call is kept, not discarded.** parse_funccall above reports UnknownFunction and
    // throws the node away, which is right for a misspelled name and wrong here: the overload set is
    // filled by the declaration pass, and a use site inside a struct property initializer is parsed
    // *during* that pass. so resolution is left to the fixpoint, which reports whatever never
    // resolved - the same standing an ordinary forward reference inside a body has
    //
    // null only when the operands are not usable at all
    AST::FunctionCallExprNode *build_operator_call(
        Parser::Payload &payload,
        const AST::Operator &op,
        AST::OpFixity fixity,
        const TokenReference &at,
        std::vector<AST::ExprNode *> operands);

    // the argument list of a call through a *value*: `$f(1, 2)`. the cursor sits on the `(`.
    //
    // no overload set and nothing to look up, so unlike parse_funccall there is no settlement to drive -
    // the callee's type says what the parameters are, and the type checker validates against it
    AST::IndirectCallExprNode *parse_indirect_call(
        Parser::Payload &payload, AST::ExprNode *callee, const TokenReference &at);

    // parses `->name(...)` / `->name<...>(...)` into an ordinary call whose first argument is the
    // receiver's address. the cursor must sit on the `(` or `<` that follows the member name
    //
    // a member call is not a node kind of its own: it is a FunctionCallExprNode with the receiver
    // prepended, which is why the monomorphizer, the pointer adjuster, the type checker and codegen
    // all handle it without a special case
    //
    // answers null two ways, and the caller has to tell them apart:
    //  - `is_call` false: the `<` was a comparison after all and the cursor has been restored to it,
    //    so `$a->count < 3` still parses as a member read
    //  - `is_call` true: this really was a call and it did not resolve. a diagnostic has been
    //    reported and the caller should abort rather than reinterpret the tokens
    AST::FunctionCallExprNode *parse_member_call(
        Parser::Payload &payload,
        AST::ExprNode *receiver,
        const TokenReference &member_token,
        bool &is_call);

    // true when the cursor sits on a call used as a statement: an optionally namespace-qualified
    // name followed by `(` or by explicit type arguments. `mem::free($p);` and `box<int32>(1);`
    // are both calls that the bare `{identifier, open_paren}` test misses, which left the
    // statement form of every qualified and every explicitly-parameterised call unparseable
    bool starts_call_statement(Parser::Payload &payload);

    // true when the cursor sits on `$f(...)` used as a statement - a call through a callable *value*.
    // its own predicate rather than an arm of starts_call_statement, which is anchored on an identifier
    // because a *name* is what a direct call begins with; named so the statement dispatch reads as a
    // list of predicates rather than a mix of predicates and inline token sequences
    bool starts_indirect_call_statement(Parser::Cursor &cursor);
};

#endif