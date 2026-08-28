#ifndef PARSERPAYLOAD_H
#define PARSERPAYLOAD_H

#pragma once

#include "Parser/ParserCursor.h"
#include "AST/ASTContext.h"
#include "AST/ASTCollector.h"

namespace Parser
{
    // which of a module's three passes over its tokens this walk is. each one runs over *every* file
    // before the next starts, one level of declaration dependency per pass: a type name has to be
    // known before a signature that mentions it is read, and a signature has to be registered before
    // a body that calls it is read. that is what makes a program independent of the order its files
    // were listed in (Parser::ModuleParser::parse_module)
    enum class Pass
    {
        // every `struct` name, as a namespace symbol, plus its type-parameter list - for a generic
        // type the arity is part of the identity a name denotes, and an application is checked
        // against it. nothing else: a constraint atom may name a type this pass has not reached, so
        // resolving one is left to the pass below
        t_type_names,

        // the declaration surface: function, method and constructor signatures, struct properties
        // and the synthesized constructor. member bodies are skipped whole - they call
        // things this pass is still collecting, and a file parsed earlier would not see them
        t_declarations,

        // the bodies, attached to the declarations the pass above registered
        t_bodies,
    };

    struct Payload
    {
        Cursor cursor;
        AST::Context context;
        AST::Collector &collector;

        // carried here rather than passed as an argument for the same reason AST::Context carries
        // `self_struct_ptr` and `return_type_ptr`: it flows downward through a parser that is a set
        // of free functions calling each other, and only some of them in the chain care. it was
        // spelled three ways before - a StructPass enum, a `bool symbol_only`, and which entry
        // function you called - so every call site in between had to convert or forward it
        Pass pass = Pass::t_bodies;

        // is this a `module.eco` rather than a `.eco` source file?
        //
        // a manifest is **Echo**, read by this same lexer and this same attribute parser into a scratch
        // bundle - which is what makes the format honest, and is also why one flag is needed. The two
        // grammars accept different attribute names, and each has to report against *its own* set:
        // `#[source:]` in a manifest is worth an "expected one of" naming only what a manifest takes, and
        // not a list naming `#[inline]`, which is legal nowhere near it. So Parser::parse_attribute defers
        // to Parser::read_manifest_attributes here rather than answering first with the wrong vocabulary.
        //
        // the *value* grammar does not split on this flag and must not start to: what an attribute value
        // may say is one question (Parser::parse_attribute_value), and which attribute names mean
        // something here is another
        bool is_manifest = false;

        // reports the token the cursor is sitting on as unexpected where `expected` was wanted
        // every parser needs this and all three of the pieces it takes - the cursor for the token,
        // the context to locate it, the collector to record it - already live here
        //
        // at end of input there is no token to sit on, and `Cursor::current` asserts rather than
        // answering - so an unterminated body reached this and aborted the compiler instead of
        // reporting the brace it was missing. the *last* token is the right place to point at: it is
        // where the input ran out, which is the only thing anyone can say about a truncated file
        void collect_unexpected_token(Token::Type expected) {
            if (!cursor.is_done()) {
                collector.collect_issue<AST::Issue::UnexpectedToken>(
                    context.code_ref(cursor.current()), expected, cursor.current().type());
                return;
            }

            collector.collect_issue<AST::Issue::UnexpectedToken>(
                cursor.is_empty() ? context.code_ref() : context.code_ref(cursor.last()),
                expected, Token::Type::t_unknown);
        }

        // answers whether the cursor is on the token this position requires, and when it is not, reports
        // it and recovers to the next statement. the recovery ritual a positional check shares, so the
        // sites differ only in what they expect - here rather than in one parser, because every one of
        // them performs it and a copy in each parser would drift
        //
        // deliberately does *not* consume the token it matched: the caller decides, and a member body's
        // opening brace is looked at without being skipped
        bool expect_token(Token::Type expected) {
            if (cursor.is_type(expected)) {
                return true;
            }

            collect_unexpected_token(expected);
            cursor.try_skip_to_next_statement();

            return false;
        }
    };
};



#endif
