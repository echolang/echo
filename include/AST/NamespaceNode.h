#ifndef NAMESPACENODE_H
#define NAMESPACENODE_H

#pragma once

#include "ASTNode.h"
#include "ASTNamespace.h"
#include "../Lexer.h"

namespace AST 
{
    class NamespaceNode : public Node
    {
    public:
        static constexpr NodeType node_type = NodeType::n_namespace;
        
        TokenSlice namespace_tokens;
        const Namespace* ast_namespace;

        NamespaceNode(const TokenSlice& token_slice, const Namespace* ns) :
            namespace_tokens(token_slice),
            ast_namespace(ns) 
        {};

        ~NamespaceNode() {};

        const std::string node_description() override { 
            return "ns<" + ast_namespace->name() + ">";
        }

        void accept(Visitor& visitor) override {
            visitor.visitNamespace(*this);
        }
    };
};


#endif