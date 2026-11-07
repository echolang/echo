#ifndef EXTENSIONPARSER_H
#define EXTENSIONPARSER_H

#pragma once

#include "Parser/ParserCursor.h"
#include "Parser/ParserPayload.h"
#include "Parser/TypeParser.h"
#include "Parser/VisibilityParser.h"

#include "Token.h"

namespace Parser
{
    // `extension Type { ... }` - contextual at file or namespace scope, like `type` and `map`.
    // identifier "extension", a type name, then `{`. `extension` is an ordinary identifier
    // everywhere else
    inline bool starts_extension(const Cursor &cursor)
    {
        if (!cursor.is_type(Token::Type::t_identifier) || cursor.current().value() != "extension") {
            return false;
        }

        Cursor scan = cursor;
        scan.skip();
        return skip_type_shape(scan) && scan.is_type(Token::Type::t_open_brace);
    }

    // the dispatch both file-scope walks share. inside a `{ }` it is refused with the same
    // sentence, so the two passes consume the same tokens
    void parse_file_scope_extension(
        Payload &payload,
        const std::optional<TokenReference> &block_token,
        const VisibilityPrefix &visibility);
};

#endif
