#ifndef SYMBOLPARSER_H
#define SYMBOLPARSER_H

#pragma once

#include "AST/ScopeNode.h"
#include "AST/ASTContext.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // registers every type name a file declares **at file or struct scope** as a namespace symbol, and
    // nothing else. runs over every file of a module before parse_symbols does, so that a member's or a
    // parameter's type resolves no matter which file - or which line - declares it
    //
    // a type written inside a `{ }` block is deliberately left alone: its name belongs to the block's
    // lexical namespace, which only the two later passes can mint, and it is visible in one block of one
    // file - so there is no order for this pass to make it independent of. parse_typedecl publishes it
    //
    // the declaration pass needs that: it reads property types, and an unresolved *unqualified* type
    // name is not a diagnostic, it silently becomes `unknown`. so a struct name arriving late would
    // not fail, it would quietly produce a wrong layout
    //
    // deliberately silent - it validates nothing. every malformed declaration it walks past is
    // reported by parse_symbols and again by the body pass, and a third voice would only be noise
    void parse_type_names(Payload &payload);

    void parse_symbols(Payload &payload);

    // the declarations of a region, and only the declarations - statements are stepped over. the one
    // walk the declaration pass uses, whether the region is a whole file or one `{ }` block, so the two
    // cannot disagree about what a declaration pass collects
    //
    // `block_token` is the brace the region opens at, and passing it does two things: the region gets
    // that block's lexical namespace, so a declaration inside it belongs to the block, and the walk
    // stops at the matching `}` instead of at end of file. nullopt is a whole file
    //
    // it *descends* rather than skipping, which is what makes a block-local declaration visible to a
    // call written above it: Parser::parse_funccall reports UnknownFunction and discards the node
    // immediately, so a name not yet registered when the body pass reaches the call is gone for good
    //
    // `scope_owner` is the attribute this region was written inside, when the region opened at a `{` glued
    // to an attribute's `]` rather than standing on its own. It is recorded on every attribute the region
    // holds and read by nothing but the manifest - a `module.eco` is Echo, walked by this very function
    void parse_declaration_surface(
        Payload &payload,
        std::optional<TokenReference> block_token = std::nullopt,
        AST::AttributeNode *scope_owner = nullptr
    );
};


#endif
