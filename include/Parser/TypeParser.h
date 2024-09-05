#ifndef TYPEPARSER_H
#define TYPEPARSER_H

#pragma once

#include "AST/ASTContext.h"
#include "AST/TypeNode.h"
#include "Parser/ParserPayload.h"

namespace AST
{
    class FunctionDeclNode;
};

namespace Parser
{
    bool can_parse_type(Payload &payload);

    // true when the cursor sits on a namespace qualified type followed by a variable name,
    // e.g. `a::b::Foo $x`. lets the statement dispatch tell a qualified var declaration apart
    // from a qualified function call `a::foo()`. pure lookahead, the cursor is not moved
    bool starts_qualified_vardecl(Payload &payload);

    // true when the cursor sits on a borrow declaration, `int32& $r` / `int32 & $r`. the lexer
    // only emits t_ref when the `&` abuts a name character, so the spaced form arrives as
    // t_and and both spellings have to be recognised. lives here, next to parse_ref_suffix,
    // so every statement dispatch that has to spot a declaration asks the same question -
    // the struct body used to keep its own list and silently rejected borrow properties.
    // pure lookahead, the cursor is not moved
    bool starts_borrow_vardecl(Payload &payload);

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

    // Parses an optional generic type-parameter list `<T, U, ...>` (the declaration side,
    // e.g. on a function or struct). Each parameter may carry a constraint
    // `T: atom (| atom)*` where an atom is a primitive, an alias (e.g. `numeric`) or a
    // user type. Returns the parsed parameters, or an empty vector if the cursor is not
    // positioned at a `<`. Consumes through the closing `>`.
    std::vector<ParsedTypeParam> parse_type_param_list(Payload &payload);

    // turns parsed parameters into owned declarations installed on their owner, stamping each
    // one's ordinal and owner. idempotent across the symbol and full parser passes: an unchanged
    // list reuses the declarations already installed, so a parameter has exactly one declaration
    // no matter how often its owner is re-parsed.
    //
    // a constructor or method of a generic struct must NOT call this — it shares the struct's
    // declarations by copying the pointer vector, which is what lets one substitution bind the
    // parameters mentioned in both the owner's and the member's types
    void declare_type_parameters(Payload &payload, AST::ComplexType &owner, const std::vector<ParsedTypeParam> &parsed);
    void declare_type_parameters(Payload &payload, AST::FunctionDeclNode &owner, const std::vector<ParsedTypeParam> &parsed);
};


#endif