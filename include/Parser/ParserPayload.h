#ifndef PARSERPAYLOAD_H
#define PARSERPAYLOAD_H

#pragma once

#include "ParserCursor.h"
#include "../AST/ASTContext.h"
#include "../AST/ASTCollector.h"

namespace Parser
{
    // which of a module's three passes over its tokens this walk is. each one runs over *every* file
    // before the next starts, one level of declaration dependency per pass: a type name has to be
    // known before a signature that mentions it is read, and a signature has to be registered before
    // a body that calls it is read. that is what makes a program independent of the order its files
    // were listed in (Parser::ModuleParser::parse_module)
    enum class Pass
    {
        // every `struct` name, as a namespace symbol, and nothing else
        t_type_names,

        // the declaration surface: function, method and constructor signatures, struct properties
        // and the synthesized field-wise constructor. member bodies are skipped whole - they call
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

        // reports the token the cursor is sitting on as unexpected where `expected` was wanted.
        // every parser needs this and all three of the pieces it takes - the cursor for the token,
        // the context to locate it, the collector to record it - already live here
        void collect_unexpected_token(Token::Type expected) {
            collector.collect_issue<AST::Issue::UnexpectedToken>(
                context.code_ref(cursor.current()), expected, cursor.current().type());
        }
    };
};



#endif