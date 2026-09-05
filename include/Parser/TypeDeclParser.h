#ifndef TYPEDECLPARSER_H
#define TYPEDECLPARSER_H

#pragma once

#include "AST/TypeDeclNode.h"
#include "Parser/ParserPayload.h"

namespace Parser
{
    // `struct`, `class`, `interface` and `enum` open the same declaration. one predicate rather than the
    // token spelled at each of the four dispatch sites, so a site cannot be taught about one keyword and
    // not the others - which for the type-name pass would mean a name never becoming a namespace symbol,
    // and an unqualified use of it silently typing as `unknown` rather than as a diagnostic
    inline bool starts_typedecl(const Cursor &cursor) {
        return cursor.is_type(Token::Type::t_struct)
            || cursor.is_type(Token::Type::t_class)
            || cursor.is_type(Token::Type::t_interface)
            || cursor.is_type(Token::Type::t_enum);
    }

    // `extern struct Name;` - the incomplete-type declaration. `extern` then a type keyword, so
    // `extern function<...>` (a C function-pointer *type*) and `extern { }` (a C surface block)
    // stay the other two productions. class/interface/enum after `extern` match so the parser
    // can refuse them by name rather than falling through to the block
    inline bool starts_extern_typedecl(const Cursor &cursor) {
        return cursor.is_type(Token::Type::t_extern)
            && (cursor.peek_is_type(1, Token::Type::t_struct)
                || cursor.peek_is_type(1, Token::Type::t_class)
                || cursor.peek_is_type(1, Token::Type::t_interface)
                || cursor.peek_is_type(1, Token::Type::t_enum));
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

        if (cursor.is_type(Token::Type::t_enum)) {
            return AST::ComplexTypeKind::t_enum;
        }

        return AST::ComplexTypeKind::t_struct;
    }

    // a struct or class body is walked in *both* the declaration and the body pass, by the same code,
    // so the two cannot disagree about where a member ends - they differ only in what they keep, which
    // `payload.pass` tells them
    AST::TypeDeclNode *parse_typedecl(Payload &payload);

    // **a type written where a type parameter is visible.** the third case of the rule parse_funcdecl
    // already applies to a nested function and parse_closure to a closure. true when it refused,
    // and the caller still owes recovery - skip a body, drain attributes - because the two
    // productions have already consumed different tokens
    bool refuse_type_in_generic_body(Payload &payload, const TokenReference &name_token);

    // the three type-declaration attributes, one call so parse_typedecl and parse_opaque_typedecl
    // cannot disagree about which are bound
    void bind_type_decl_attributes(Payload &payload, AST::TypeDeclNode &node);

    // **the one publisher of a type name into a namespace**, and find-before-create because
    // Namespace::push_symbol replaces the slot and frees what was there: a second declaration of the
    // name would leave the first node's Symbol dangling and hand codegen two TypeDeclNodes for one type.
    // returns whether this node now holds the name - false when a *different* type already did, which
    // both callers leave to parse_typedecl's redeclaration report at the duplicate's own name token
    bool publish_type_symbol(Payload &payload, AST::Namespace &ns, AST::TypeDeclNode &node);
};


#endif
