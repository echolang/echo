#ifndef ASTDECLARATIONSITE_H
#define ASTDECLARATIONSITE_H

#pragma once

#include "Token.h"

#include <cstddef>
#include <functional>

namespace AST
{
    // the position a declaration is *written* at, and the one identity every parse pass agrees on. a
    // module is tokenized once and every pass walks identical indices, so this is exact - and it is
    // available before the declaration has been read, which the name is not: with overloads a name
    // denotes a set, and the reuse decision has to be made at the name token, long before the
    // parameter list that would tell the overloads apart has been parsed
    //
    // two things key on it. AST::FunctionRegistry reconciles the declaration passes on it (which
    // token that is, is FunctionDeclNode::declaration_site_token()'s answer), and
    // AST::NamespaceManager keys a block's lexical namespace on its opening brace - both need one
    // spelling of "written here", so it lives here rather than inside either of them
    struct DeclarationSite
    {
        const TokenCollection *tokens;
        size_t index;

        bool operator==(const DeclarationSite &other) const {
            return tokens == other.tokens && index == other.index;
        }
    };

    struct DeclarationSiteHash
    {
        size_t operator()(const DeclarationSite &site) const {
            return std::hash<const TokenCollection *>{}(site.tokens) ^ (std::hash<size_t>{}(site.index) << 1);
        }
    };

    // the site key a token denotes. the one spelling of the identity, so no two readers can construct
    // it differently
    inline DeclarationSite make_declaration_site(const TokenReference &token) {
        return DeclarationSite { &token.get_collection_ref(), token.get_handle() };
    }
};

#endif
