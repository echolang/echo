#ifndef ATTRIBUTEPARSER_H
#define ATTRIBUTEPARSER_H

#pragma once

#include "AST/ASTAttributeReader.h"
#include "AST/AttributeNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // one `#[...]`, and only the brackets - a `{` that follows is the *caller's* to read, since what a
    // brace after an attribute means is a question about the region being walked and not about the
    // attribute. `scope_owner` is the attribute this one was written inside, recorded on the node
    AST::AttributeNode *parse_attribute(
        Parser::Payload &payload,
        AST::AttributeNode *scope_owner = nullptr
    );

    // the value an attribute carries, or null having reported that it carries none. every consumer of
    // a valued attribute starts here, and what it does with the value afterwards is
    // AST::AttributeReader's question
    const AST::AttributeValue *attribute_value_of(
        Parser::Payload &payload,
        AST::AttributeNode *attribute,
        const std::string &attribute_name
    );

    // drains a reader's refusals into the collector as located issues.
    //
    // the *source file* half of AST::AttributeReader's two channels - a manifest drains the same
    // refusals into its own `<file>:<line>: <what>` string, which is why the reader accumulates them
    // rather than reporting them itself
    void report_attribute_refusals(Parser::Payload &payload, const AST::AttributeReader &reader);

    // reads an attribute's one value through a fresh reader and hands back what survived.
    //
    // `read` is given that reader and the written value: it says what the value has to be - a string for
    // free text, a name for a closed vocabulary - and may refuse further through the reader, which is the
    // whole of what the consumers of a valued attribute differ in.
    //
    // **the drain lives here and nowhere else.** A refusal is what makes the answer absent, so a consumer
    // that has to remember to report one is a consumer that says nothing on the day it forgets
    template <class Read>
    std::optional<std::string> read_attribute_value(
        Parser::Payload &payload,
        AST::AttributeNode *attribute,
        const std::string &attribute_name,
        Read read
    )
    {
        const AST::AttributeValue *written = attribute_value_of(payload, attribute, attribute_name);

        if (written == nullptr) {
            return std::nullopt;
        }

        AST::AttributeReader reader(attribute_name);
        std::optional<std::string> value = read(reader, *written);
        report_attribute_refusals(payload, reader);

        return value;
    }
};

#endif
