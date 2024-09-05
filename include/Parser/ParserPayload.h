#ifndef PARSERPAYLOAD_H
#define PARSERPAYLOAD_H

#pragma once

#include "ParserCursor.h"
#include "../AST/ASTContext.h"
#include "../AST/ASTCollector.h"

namespace Parser
{
    struct Payload
    {
        Cursor cursor;
        AST::Context context;
        AST::Collector &collector;

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