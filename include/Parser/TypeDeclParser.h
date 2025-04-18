#ifndef TYPEDECLPARSER_H
#define TYPEDECLPARSER_H

#pragma once

#include "AST/TypeDeclNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // `struct`, `class` and `interface` open the same declaration. one predicate rather than the token
    // spelled at each of the three dispatch sites, so a site cannot be taught about one keyword and not
    // the others - which for the type-name pass would mean a name never becoming a namespace symbol,
    // and an unqualified use of it silently typing as `unknown` rather than as a diagnostic
    inline bool starts_typedecl(const Cursor &cursor) {
        return cursor.is_type(Token::Type::t_struct)
            || cursor.is_type(Token::Type::t_class)
            || cursor.is_type(Token::Type::t_interface);
    }

    // which storage class the keyword under the cursor declares. only meaningful when
    // starts_typedecl is true
    inline AST::ComplexTypeKind typedecl_kind(const Cursor &cursor) {
        if (cursor.is_type(Token::Type::t_class)) {
            return AST::ComplexTypeKind::t_class;
        }

        if (cursor.is_type(Token::Type::t_interface)) {
            return AST::ComplexTypeKind::t_interface;
        }

        return AST::ComplexTypeKind::t_struct;
    }

    // a struct or class body is walked in *both* the declaration and the body pass, by the same code,
    // so the two cannot disagree about where a member ends - they differ only in what they keep, which
    // `payload.pass` tells them
    AST::TypeDeclNode *parse_typedecl(Payload &payload);
};


#endif
