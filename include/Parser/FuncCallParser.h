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
    // **how a call finds its candidates** - the one thing that differs between the three spellings, and
    // the reason they are one production rather than three. a call is otherwise a name, an argument
    // list and a settlement whichever way it was written
    struct CallLookup
    {
        // a free call, `foo(...)` or `a::b::foo(...)`: the namespace written before the name, or null
        // for the one the call is written in. the registry searches outward from it
        const AST::Namespace *ns = nullptr;

        // `Type::f(...)`: the type whose *static* overload set answers, and nothing walks outward from
        // it. that omission is what stops a static call from quietly meaning an enclosing free function
        AST::ValueType static_owner = AST::ValueType::make_unknown();

        // `.f(...)`: nothing names the owner yet - the destination will. so there is no lookup to do,
        // no candidates to find, and **no unknown name to report**: an empty set here is a not-yet that
        // AST::CallResolver::settle answers with the retryable t_unknown_name, and the diagnostic if it
        // never resolves belongs to the monomorphizer's finalizing sweep, which knows it ran out of rounds
        //
        // **the `.` itself rather than a flag beside it**: it is where every diagnostic about a
        // shorthand points, and it is also what the node answers `is_shorthand_static_call()` from - so
        // there is no window in which a call is known to be one and has no token to be reported at
        std::optional<TokenReference> shorthand_dot;

        // `T(...)`: the type being constructed, usually a type parameter that substitution will
        // make concrete. unknown for every other spelling. candidates come from the registry
        // under the type's name, and an undetermined owner is a not-yet rather than an unknown name
        AST::ValueType constructed_type = AST::ValueType::make_unknown();
    };

    AST::FunctionCallExprNode *parse_funccall(
        Parser::Payload &payload,
        const AST::Namespace *requested_namespace = nullptr,
        bool *out_is_call = nullptr,
        const CallLookup &lookup = {}
    );

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
        std::vector<AST::ExprNode *> operands
    );

    // the argument list of a call through a *value*: `$f(1, 2)`. the cursor sits on the `(`.
    //
    // no overload set and nothing to look up, so unlike parse_funccall there is no settlement to drive -
    // the callee's type says what the parameters are, and the type checker validates against it
    AST::IndirectCallExprNode *parse_indirect_call(
        Parser::Payload &payload,
        AST::ExprNode *callee,
        const TokenReference &at
    );

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
        bool &is_call
    );

    // true when the cursor sits on a call used as a statement: an optionally namespace-qualified
    // name followed by `(` or by explicit type arguments. `mem::free($p);` and `box<int32>(1);`
    // are both calls that the bare `{identifier, open_paren}` test misses, which left the
    // statement form of every qualified and every explicitly-parameterised call unparseable
    bool starts_call_statement(Parser::Payload &payload);

    // **a statement rooted in a compile-time constant**: `std::io::stdout->write('hi');`.
    //
    // its own predicate for starts_indirect_call_statement's reason - the two above are anchored on
    // an identifier followed by `(` or `<`, and this one is an identifier followed by `->`. it became
    // spellable when a constant became an expression rather than a folded value: `stdout` is
    // `stream(1)` copied in here, and what follows it is an ordinary member chain, the same one the
    // `$var ->` statement form already reads.
    //
    // deliberately only `->`. `LIST[0] = 1;` would be a write into a copy of the constant's
    // expression that nothing can observe afterwards, which is worth refusing rather than accepting
    bool starts_constant_chain_statement(Parser::Payload &payload);

    // **`Session::$count = 1;` at a statement head** - a `$name` behind a namespace-or-type prefix,
    // which is the one operand shape no other statement branch is anchored on. it answers on the
    // *shape* only; whether the prefix names a type is settled inside the expression parse
    bool starts_static_property_statement(Parser::Payload &payload);

    // true when the cursor sits on `$f(...)` used as a statement - a call through a callable *value*.
    // its own predicate rather than an arm of starts_call_statement, which is anchored on an identifier
    // because a *name* is what a direct call begins with; named so the statement dispatch reads as a
    // list of predicates rather than a mix of predicates and inline token sequences
    bool starts_indirect_call_statement(Parser::Cursor &cursor);
};

#endif
