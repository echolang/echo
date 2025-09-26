#ifndef SCOPEPARSER_H
#define SCOPEPARSER_H

#pragma once

#include "AST/ScopeNode.h"
#include "AST/ASTContext.h"
#include "AST/ExprNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // parses statements until the scope's closing brace, into a scope of its own making
    //
    // `block_token` is the `{` this scope opens at, and every caller that has one must pass it: it is
    // the identity of the block's lexical namespace, which is what makes a declaration written in here
    // belong to this block rather than to the enclosing namespace. only the file root has none
    //
    // `seed_declarations` are declarations the scope is to open with, in the order they are to be seen -
    // a constructor's `$this`, or a `foreach`'s `$k` and `$el`: names the body can read but nobody wrote
    // in it. they are registered before the first statement is parsed, and that is the whole of what
    // they are for: a name has to be resolvable while the statements that read it are being parsed
    // (Parser::parse_varexpr asks ScopeNode::lookup_variable), and a caller cannot register one after
    // the fact. their *storage* does not care where they land - StmtCodegen::gen_scope seats every
    // declaration's slot at scope entry and ScopeNode::clone clones them before the statements - but a
    // seed carrying an **initializer** does, that being a statement and running where it was written.
    // seeds open the scope for both reasons at once, which is why neither caller has to choose
    // (AST::declare_constructor_this states the second one for a class constructor's `$this`)
    AST::ScopeNode &parse_scope(
        Payload &payload,
        std::optional<TokenReference> block_token = std::nullopt,
        std::vector<AST::VarDeclNode *> seed_declarations = {});

    // the tail of a call used as a statement: appends it to `scope` and consumes the semicolon that
    // has to follow. shared by the free-call branch of parse_scope and the `$obj->m();` branch of
    // parse_varexpr, so the two cannot disagree about the terminator - they already did, one
    // checking t_semicolon by hand and the other going through the vardecl end-token rules
    void finish_call_statement(Payload &payload, AST::ScopeNode &scope, AST::ExprNode *call);

    // the tail of a statement whose postfix chain is rooted in a *call* rather than in a name -
    // `first(&$o)->bump(1);` or `first(&$o)->x = 42;`. the sibling of the `$var ->` path in
    // Parser::parse_varexpr, and deliberately no more than the two shapes a statement can be: the
    // chain ended in a call and stands on its own, or it named storage and something is written to it
    void finish_place_statement(Payload &payload, AST::ScopeNode &scope, AST::ExprNode *target);
};



#endif