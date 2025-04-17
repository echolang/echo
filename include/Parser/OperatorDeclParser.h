#ifndef OPERATORDECLPARSER_H
#define OPERATORDECLPARSER_H

#pragma once

#include "AST/ASTOps.h"
#include "AST/FunctionDeclNode.h"
#include "Parser/ParserPayload.h"
#include "Parser/TypeParser.h"

#include <optional>
#include <string>
#include <vector>

namespace Parser
{
    // does an `operator` declaration start here? the sole owner of the question, so the four walks
    // that have to agree about it - the type-name pass, the declaration surface, the statement
    // dispatch and the struct member walk - cannot come to four answers. the same role
    // starts_funcdecl and starts_vardecl play
    bool starts_operatordecl(Parser::Cursor &cursor);

    // the shape of a declaration, read off the tokens without parsing any of it. shared by the
    // type-name pass, which wants only this, and by parse_operatordecl, which wants this and then
    // everything after it
    //
    //   operator [ '<' type-params '>' ] [ '(' <int> ',' (left|right) ')' ]
    //       ( '(' params ')' '[' [ params ] ']'          // index (the second operand is bracketed)
    //       | '(' params ')' SYM [ '(' params ')' ]      // infix (two groups) or suffix (one)
    //       | SYM '(' params ')' )                       // prefix
    //       ':' <type> <body>
    struct OperatorHeader
    {
        // the symbol split into the ordinary tokens it lexes as, by value - {"avg"}, {"!","!"}
        std::vector<std::string> symbol_tokens;

        // the symbol as written, which is those values concatenated
        std::string spelling;

        // the first token of the symbol, which is where a diagnostic about the symbol points and
        // where the declaration's virtual name token is positioned
        std::optional<TokenReference> symbol_token;

        AST::OpFixity fixity = AST::OpFixity::t_infix;

        // the `(` of the **left** operand list, when the declaration was written with one. the header
        // walks over that list on its way to the symbol - the fixity is not known until the symbol
        // has been read - so where it stood is recorded rather than found a second time: re-deriving
        // it means spelling the "an integer after the paren is a precedence clause, not a parameter
        // list" rule twice, and those two copies have to be changed together
        std::optional<Cursor::Snapshot> left_params;

        // the `[` of an index operator's bracketed operand list, when the declaration is one. the
        // same role `left_params` plays, and recorded for the same reason: the header walks past it
        // on its way to the `:` and the fixity is only settled once it has
        std::optional<Cursor::Snapshot> index_params;

        // the type parameters of an `operator<T>`, parsed **once**, here. the list's grammar has one
        // owner - Parser::parse_type_param_list, which is what a function's declaration uses - and the
        // header has to walk it anyway on its way to the symbol, so it keeps what that walk produced
        // rather than recording where to go back to
        //
        // it is pass-aware underneath: the type-name pass deliberately walks a constraint's atoms
        // without resolving them, because an atom may name a type no pass has registered yet. so a
        // header read in that pass carries names only, which is all that pass declares
        std::vector<ParsedTypeParam> type_params;

        // the `(N, assoc)` clause, when one was written. absent is not the same as defaulted: a
        // second declaration of one symbol writing a *different* number is a conflict, while one
        // writing none at all is not
        std::optional<int> precedence;
        std::optional<AST::OpAssociativity> associativity;

        // could the shape be read at all? `valid` implies `symbol_token` and `spelling`: it is only
        // set once a symbol has been collected, which is what lets every reader below dereference the
        // optional rather than guard it
        bool valid = false;
    };

    // reads the header and leaves the cursor on the token after the symbol, having reported anything
    // wrong with it. `header.valid` is false when the shape could not be read at all
    //
    // the cursor must be on the `operator` keyword
    OperatorHeader read_operator_header(Payload &payload);

    // publishes a header's symbol, fixity and precedence into the bundle's operator table, reporting
    // a precedence that contradicts an earlier declaration of the same symbol
    //
    // called from the **type-name pass**, which is a pass earlier than every other declaration
    // publishes in, and deliberately so: the expression parser has to know whether a symbol is an
    // operator before it parses a single use site, and the *declaration* pass already parses
    // expressions itself - a struct property's `= ...` initializer. publishing there would make a
    // property initializer's operators depend on which file was walked first
    void publish_operator_symbol(Payload &payload, const OperatorHeader &header);

    // parses a declaration whose signature is registered in the declaration pass and whose body is
    // parsed in the body pass, read off `payload.pass` exactly as parse_funcdecl reads it
    //
    // every refusal it owns reports at the declaration and then consumes it whole, so a caller that
    // cannot accept an operator here - the struct member walk - calls this rather than refusing it
    // itself: the diagnostic and the recovery then have one owner, as they do for a free function
    // marked `#[implicit]`
    AST::FunctionDeclNode *parse_operatordecl(Payload &payload);
};

#endif
