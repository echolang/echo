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
        Cursor &cursor;
        AST::Context &context;
        AST::Collector &collector;
    };
};



#endif