#ifndef NAMESPACEDECLNODE_H
#define NAMESPACEDECLNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTNamespace.h"
#include "../Lexer.h"

namespace AST 
{
    class NamespaceDeclNode : public Node
    {
    public:
        static constexpr NodeType node_type = NodeType::n_namespace_decl;
        
        TokenSlice namespace_tokens;
        const Namespace* namespace_decl;

        NamespaceDeclNode(const TokenSlice& token_slice, const Namespace* ns) :
            namespace_tokens(token_slice),
            namespace_decl(ns) 
        {};

        ~NamespaceDeclNode() {};

        const std::string node_description() override { 
            return "namespace " + namespace_decl->name();
        }

        void accept(Visitor& visitor) override {
            visitor.visitNamespaceDecl(*this);
        }
    };
};

#endif