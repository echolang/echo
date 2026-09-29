#ifndef TYPEPARSER_H
#define TYPEPARSER_H

#pragma once

#include "AST/ASTContext.h"
#include "AST/ASTTypeParam.h"
#include "AST/TypeNode.h"
#include "Parser/ParserPayload.h"
#include "Parser/TypeParamParser.h"

namespace AST
{
    class FunctionDeclNode;
    class Namespace;
    class Symbol;
};

namespace Parser
{
    bool can_parse_type(Payload &payload);

    // an unqualified type name, after a file-local `use`. the type grammar and the static-owner
    // speculation share this so `use geometry::Point` then `Point::origin()` and `Point $p` agree
    AST::Symbol *find_unqualified_type(Payload &payload, const std::string &name, const AST::Namespace &from);

    // does the callable type `function<R(P...)>` start at `offset`? the third of the three things the
    // `function` keyword introduces, and the only one that is a type - the sibling of
    // Parser::starts_funcdecl and Parser::starts_closure_literal, which own the other two
    //
    // it lives here rather than beside them because it is a production of the type grammar, and this
    // file owns that grammar: can_parse_type, skip_type_shape and parse_value_type all ask it
    bool starts_callable_type(Cursor &cursor, size_t offset = 0);

    // does the C function-pointer type `extern function<R(P...)>` start at `offset`? `extern`
    // then the callable type, and the angle bracket is what keeps `extern function foo()` from
    // being a type - that is still a missing `{`
    bool starts_c_function_type(Cursor &cursor, size_t offset = 0);

    // true when the cursor sits on a variable declaration in any of its spellings - inferred
    // (`$x = ...`), typed (`int32 $x`), qualified, generic, borrowed, const or ptr. the one owner
    // of "what a declaration looks like", so a scope body and a struct body cannot disagree about
    // it; a token list in each parser would silently lag behind the other
    //
    // the question is answered by scanning the *type grammar* and looking at what follows it,
    // rather than by enumerating token sequences. an enumeration needs an arm per spelling and had
    // none for a generic application, so `Q<int32> $q` and `struct H { Q<int32> $i; }` did not
    // parse - and the three arms that did exist did not compose, so `a::b::Foo& $r` matched none of
    // them. a scan has one arm per *grammar production* instead, which is the thing that has a
    // fixed number of cases
    //
    // the one exception is a leading `const`, which begins a *constant* declaration as readily as a variable
    // one - so this defers to starts_constdecl below rather than claiming it. The two are a partition
    //
    // pure lookahead: the scan moves the cursor and restores it before returning
    bool starts_vardecl(Payload &payload);

    // true when the cursor sits on a **compile-time constant** declaration: `const NAME = ...` or
    // `const <type> NAME = ...`, where NAME is a bare identifier.
    //
    // one question split from starts_vardecl on the one token that separates them - a `$`. Both spellings
    // begin with `const`, and what follows the type says which it is: a variable has storage in the scope
    // it was written in, a constant has none and is copied to each of its use sites. Every dispatch site
    // asks this one **before** starts_vardecl, and starts_vardecl defers to it on a leading `const` - so the
    // two answer yes to disjoint sets of inputs rather than relying on the dispatch order alone
    //
    // it lives here for the reason starts_callable_type does: the answer is a scan of the type grammar, and
    // this file owns that grammar
    //
    // pure lookahead, same as its sibling: the scan moves the cursor and restores it before returning
    bool starts_constdecl(Payload &payload);

    // with the cursor **past** the `const`: is this the untyped spelling, `const NAME = ...`?
    //
    // shared by starts_constdecl and Parser::parse_constdecl, which would otherwise each carry their own copy
    // of the test - and a parser that disagreed with the predicate that routed it there would read the name as
    // a type and then report a missing one
    bool constdecl_omits_its_type(Cursor &cursor);

    AST::TypeNode *parse_type(Payload &payload);

    // silent: a name the type grammar would read as a primitive (`usize`, `int32`, …).
    // parse_static_owner needs this before it calls parse_type, which reports an unknown
    // identifier - speculation cannot afford that
    bool is_primitive_type_name(const std::string &name);

    // the constraint half of the type-parameter grammar, `: atom (| atom)*`, on its own - so
    // an interface's associated type (`type Iter : contract::iterator<V>`) and a `where`
    // clause are constrained by the same rule a type parameter is, rather than by a second
    // scanner that could drift from it. no-op and true when the cursor is not on a ':';
    // false when it reported and gave up.
    //
    // `resolve_atoms` is the caller's decision, not the pass's. false walks the shape and stores
    // nothing: the type-name pass, because the types are not registered yet, and the parameter-list
    // scan, because `T : Cmp<T>` names T before T is a declaration. true resolves. the list scan
    // re-reads through install_type_parameters once the names are in scope
    bool parse_constraint_atoms(Payload &payload, ParsedTypeParam &param, bool resolve_atoms);

    // parses an optional generic type-parameter list `<T, U, ...>` (the declaration side,
    // e.g. on a function or struct). Each parameter may carry a constraint
    // `T: atom (| atom)*` where an atom is a primitive, an alias (e.g. `numeric`), the
    // kind predicate `class`, or a user type. Returns the parsed parameters, or an empty
    // vector if the cursor is not positioned at a `<`. Consumes through the closing `>`
    std::vector<ParsedTypeParam> parse_type_param_list(Payload &payload);
};


#endif
