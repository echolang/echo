#ifndef FUNCDECLPARSER_H
#define FUNCDECLPARSER_H

#pragma once

#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/ASTContext.h"
#include "AST/FunctionDeclNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // which grammar a `function` declaration is being read under. an extern declaration accepts
    // the `function <c_symbol> as <echo_name>(...)` renaming spelling and must be bodyless;
    // everything else about the signature is parsed by the same code, so the two forms cannot
    // drift apart
    enum class FuncDeclKind
    {
        t_normal,
        t_extern,
    };

    // whether the body is parsed or left for the body pass is read off `payload.pass`
    AST::FunctionDeclNode *parse_funcdecl(Payload &payload, FuncDeclKind kind = FuncDeclKind::t_normal);

    // reads a parameter list up to and *including* its closing parenthesis, appending each parameter
    // to `decl` and declaring it in `into`. the cursor must already be past the open parenthesis
    //
    // shared with the struct parser's `constructor(...)`, which is an ordinary declaration in every
    // respect the signature is concerned with - the two used to carry a copy each, and the copies had
    // already drifted in how they recovered
    //
    // answers false when the list runs off the end of the file, having reported that at `report_at`
    // and recovered: there is no partial parameter list worth carrying on with, because the arity is
    // what a call resolves against
    bool parse_parameter_list(
        Payload &payload, AST::FunctionDeclNode &decl, AST::ScopeNode &into, const TokenReference &report_at);

    // prepends the implicit `$this` receiver to `decl`, typed `self_type` - the non-nullable borrow
    // `Foo&` (or `Foo<T>&`) every member of the struct shares - and declares it in `into` so it
    // resolves exactly as any other parameter does
    //
    // a *parameter* rather than a body-local the way a constructor's `$this` is, which is what makes
    // a member an ordinary function: mangling, cloning, the pointer adjuster and codegen all handle
    // it with no special case. shared by the method and destructor arms so the two receivers cannot
    // drift - they are the same thing, and a destructor that borrowed differently would mutate a
    // copy and free nothing
    void push_receiver_param(
        Payload &payload,
        AST::FunctionDeclNode &decl,
        AST::ScopeNode &into,
        AST::TypeNode *self_type,
        const TokenReference &at);
};


#endif