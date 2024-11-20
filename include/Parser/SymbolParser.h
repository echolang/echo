#ifndef SYMBOLPARSER_H
#define SYMBOLPARSER_H

#pragma once

#include "AST/ScopeNode.h"
#include "AST/ASTContext.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // registers every type name a file declares as a namespace symbol, and nothing else. runs over
    // every file of a module before parse_symbols does, so that a member's or a parameter's type
    // resolves no matter which file - or which line - declares it.
    //
    // the declaration pass needs that: it reads property types, and an unresolved *unqualified* type
    // name is not a diagnostic, it silently becomes `unknown`. so a struct name arriving late would
    // not fail, it would quietly produce a wrong layout.
    //
    // deliberately silent - it validates nothing. every malformed declaration it walks past is
    // reported by parse_symbols and again by the body pass, and a third voice would only be noise.
    void parse_type_names(Payload &payload);

    void parse_symbols(Payload &payload);
};


#endif