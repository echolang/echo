#ifndef OPERATORDECLPARSER_H
#define OPERATORDECLPARSER_H

#pragma once

#include "AST/ASTOps.h"
#include "AST/FunctionDeclNode.h"
#include "Parser/ParserPayload.h"

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
    //   operator [ '(' <int> ',' (left|right) ')' ]
    //       ( '(' params ')' SYM [ '(' params ')' ]      // infix (two groups) or suffix (one)
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

        // the `(N, assoc)` clause, when one was written. absent is not the same as defaulted: a
        // second declaration of one symbol writing a *different* number is a conflict, while one
        // writing none at all is not
        std::optional<int> precedence;
        std::optional<AST::OpAssociativity> associativity;

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
    AST::FunctionDeclNode *parse_operatordecl(Payload &payload);

    // consumes a declaration the caller is refusing, so the walk resumes after it rather than inside
    // its body. used by the struct member walk, which refuses one outright
    void skip_operatordecl(Payload &payload);
};

#endif
