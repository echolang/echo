#ifndef TESTDECLPARSER_H
#define TESTDECLPARSER_H

#pragma once

#include "Parser/ParserPayload.h"

namespace Parser
{
    // `test <name> { ... }` - is that what starts here?
    //
    // one token, because `test` is a keyword. Cheap enough to be asked beside `starts_typedecl` in both
    // dispatch loops rather than being folded into one of the four `starts_` predicates that scan the type
    // grammar - a test is not a declaration of a type, a constant, a variable or a function signature
    bool starts_testdecl(Cursor &cursor);

    // **a test is a function**, and that is the design rather than a shortcut: an ordinary
    // AST::FunctionDeclNode of no arguments returning `void`, at file scope, with `MemberKind::t_test` and
    // a virtual name no source could spell. Everything downstream - AST::Monomorphizer's fixpoint,
    // AST::OwnershipPass, AST::TypeChecker, the bodies loop in LLVMCompiler::compile_bundle - finds it by
    // walking what it already walks, so not one of them has an arm for a test.
    //
    // called from both walks and it has to consume the same tokens in each, which is what `symbol_only`
    // is for. In the declaration pass it registers **nothing**: a test is in no overload set, so there is
    // no signature to publish, and the pass descends into the body only so that a `struct` written inside
    // one joins the body's unspellable lexical namespace. The declaration node is minted by the body pass
    // alone, which is why nothing here reconciles on a declaration site.
    //
    // a visibility modifier is refused before this is ever called, by the one gate that owns that question
    // for every dispatch loop - see Parser::k_test_visibility_refusal
    void parse_testdecl(Payload &payload, bool symbol_only);
};

#endif
