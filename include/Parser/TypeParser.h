#ifndef TYPEPARSER_H
#define TYPEPARSER_H

#pragma once

#include "AST/ASTContext.h"
#include "AST/TypeNode.h"
#include "Parser/ParserPayload.h"

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

    // one type parameter exactly as written, before it becomes a declaration. parsing produces
    // syntax; minting the owned TypeParamDecl is the declaring step, which the owner node does
    // (see AST::declare_type_parameters) so it can stay idempotent across the two parser passes
    struct ParsedTypeParam
    {
        TokenReference name_token;
        std::vector<AST::ValueType> constraint;
        std::string constraint_spelling;

        const std::string &name() const {
            return name_token.value();
        }
    };

    // parses an optional generic type-parameter list `<T, U, ...>` (the declaration side,
    // e.g. on a function or struct). Each parameter may carry a constraint
    // `T: atom (| atom)*` where an atom is a primitive, an alias (e.g. `numeric`) or a
    // user type. Returns the parsed parameters, or an empty vector if the cursor is not
    // positioned at a `<`. Consumes through the closing `>`
    // the constraint half of that grammar, `: atom (| atom)*`, on its own - so an interface's
    // associated type (`type Iter : contract::iterator<V>`) is constrained by the same rule a type parameter
    // is, rather than by a second scanner that could drift from it. no-op and true when the cursor is not on
    // a ':'; false when it reported and gave up
    bool parse_constraint_atoms(Payload &payload, ParsedTypeParam &param);

    std::vector<ParsedTypeParam> parse_type_param_list(Payload &payload);

    // turns parsed parameters into owned declarations installed on their owner, stamping each
    // one's ordinal and owner. idempotent across the symbol and full parser passes: an unchanged
    // list reuses the declarations already installed, so a parameter has exactly one declaration
    // no matter how often its owner is re-parsed
    //
    // a constructor of a generic struct must NOT call this: it shares the struct's declarations by
    // copying the pointer vector, which is what lets one substitution bind the parameters mentioned
    // in both the owner's and the constructor's types
    void declare_type_parameters(Payload &payload, AST::ComplexType &owner, const std::vector<ParsedTypeParam> &parsed);

    // the function overload owns the whole `[inherited..., own...]` shape of
    // FunctionDeclNode::type_parameters, inherited_type_param_count included: a method of a generic
    // struct passes the owner's declarations as `inherited` and they are shared, not re-declared
    // stripping the prefix before declaring and re-prefixing after lives here rather than at the call
    // site, because the reuse rule that forces it is here. see the implementation
    void declare_type_parameters(
        Payload &payload,
        AST::FunctionDeclNode &owner,
        const std::vector<ParsedTypeParam> &parsed,
        const std::vector<AST::TypeParamDecl *> &inherited = {});
};


#endif
