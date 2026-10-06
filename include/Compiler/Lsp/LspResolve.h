#ifndef LSPRESOLVE_H
#define LSPRESOLVE_H

#pragma once

#include "AST/ASTValueType.h"
#include "Token.h"

#include <unordered_map>

namespace AST
{
    class Bundle;
    class File;
    class Node;
    class TypeDeclNode;
};

namespace Compiler
{
    namespace Lsp
    {
        // what a node in the index *means*: the declaration a use resolves to. one owner, so hover,
        // definition, references, rename and highlight name the same target

        // the declaration that wrote a type, through an instantiation to its template. null for a type
        // nothing declared (a primitive, a callable)
        AST::TypeDeclNode *type_decl_of(AST::Bundle &bundle, const AST::ValueType &type);

        // the declaration a node in the index points at. a declaration answers itself
        AST::Node *definition_target(AST::Node *node, AST::Bundle &bundle);

        // a generic instance's declaration answered as its template, so every instance of `max<T>`
        // is the one function the user wrote
        AST::Node *canonical_target(AST::Node *node);

        // definition_target then canonical_target, memoized: a references scan asks it once per
        // index entry in the workspace
        AST::Node *reference_target(
            AST::Node *node,
            AST::Bundle &bundle,
            std::unordered_map<AST::Node *, AST::Node *> &cache
        );

        // the token that names a declaration: what definition jumps to and rename rewrites
        const TokenReference *name_token_of(AST::Node *node);

        // is this file the standard library's, whether embedded (`stdlib:` paths) or read from a
        // source checkout (ordinary paths, module "stdlib": see Compiler::parse_front_end_bundle)
        bool is_stdlib_file(const AST::File &file);
    };
};

#endif
