#ifndef ATTRIBUTEPARSER_H
#define ATTRIBUTEPARSER_H

#pragma once

#include "Parser/ParserPayload.h"
#include "AST/AttributeNode.h"

namespace Parser
{
    AST::AttributeNode *parse_attribute(Parser::Payload &payload);

    // reads the single string value out of an attribute like `#[intrinsic: "llvm.sin"]`, reporting a
    // located issue and answering nullopt when the attribute is malformed. shared by every attribute
    // whose payload is one string, so they cannot diverge in how they validate - `intrinsic` and
    // `builtin` on a function, `core` on a type
    std::optional<std::string> attribute_string_value(
        Parser::Payload &payload, AST::AttributeNode *attribute, const std::string &attribute_name);
};

#endif
