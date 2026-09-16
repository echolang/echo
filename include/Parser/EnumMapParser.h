#ifndef ENUMMAPPARSER_H
#define ENUMMAPPARSER_H

#pragma once

#include "AST/TypeDeclNode.h"
#include "Parser/ParserPayload.h"
#include "Parser/VisibilityParser.h"

#include <optional>

namespace Parser
{
    // `map glfw : int32 { ... }` - contextual in an enum body, like `init` / `type`. not reserved:
    // `map<K, V>` elsewhere is the stdlib type (`map glfw :` vs `map<`). three tokens so a property
    // named `map` cannot race it, and so a struct body can refuse the same spelling rather than
    // reporting an unexpected identifier
    inline bool starts_enum_map(const Cursor &cursor) {
        return cursor.is_type(Token::Type::t_identifier)
            && cursor.current().value() == "map"
            && cursor.peek_is_type(1, Token::Type::t_identifier)
            && cursor.peek_is_type(2, Token::Type::t_colon);
    }

    // `map <name> : <type> { .case = expr; ... }` in an enum body, or
    // `map <name> : <type> for <Enum> { ... }` at file scope. consumed in both passes so they
    // agree about where the declaration ends; kept only when collect_members. functions are
    // minted after pass 2 of every file (`mint_file_enum_maps`) and planted into the origin
    // file's root in pass 3 (`plant_file_enum_maps`)
    void parse_enum_map(
        Payload &payload,
        AST::TypeDeclNode *enum_node,
        bool collect_members,
        const VisibilityPrefix &visibility = {});

    // the dispatch both file-scope walks share. a map inside a `{ }` is refused with the same
    // sentence, so the two passes consume the same tokens
    void parse_file_scope_enum_map(
        Payload &payload,
        const std::optional<TokenReference> &block_token,
        const VisibilityPrefix &visibility);

    // after pass 2 of every file: every case list and every map exists, so a call in any file
    // can resolve. asked once per file from ModuleParser, mints the maps that file wrote
    void mint_file_enum_maps(Payload &payload);

    // plant this file's map functions into the file root. asked from parse_scope when that
    // root is being built, which is when declaration_scope is the root
    void plant_file_enum_maps(Payload &payload);
};

#endif
