#ifndef DEFAULTBODYPARSER_H
#define DEFAULTBODYPARSER_H

#pragma once

#include "AST/FunctionDeclNode.h"
#include "Parser/ParserPayload.h"
#include "Token.h"

namespace Parser
{
    // is the signature just read an `interface` requirement with a receiver and no own type
    // parameters, followed by `{`?
    bool starts_default_body(const Payload &payload, const AST::FunctionDeclNode &requirement);

    // reads the body into `requirement`'s default: a template over [interface params..., Self] with `$this` a `Self&`
    bool parse_default_body(
        Payload &payload,
        AST::FunctionDeclNode &requirement,
        const TokenReference &at,
        bool symbol_only);
};

#endif
