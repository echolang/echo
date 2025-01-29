#ifndef NAMESPACENODE_H
#define NAMESPACENODE_H

#pragma once

#include "ASTNode.h"
#include "ASTNamespace.h"
#include "Lexer.h"

namespace AST 
{
    class NamespaceNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_namespace);
        
        TokenSlice namespace_tokens;
        const Namespace *ast_namespace;

        NamespaceNode(const TokenSlice &token_slice, const Namespace *ns) :
            namespace_tokens(token_slice),
            ast_namespace(ns) 
        {};

        ~NamespaceNode() {};

        const std::string node_description() override { 
            return "ns<" + ast_namespace->display_name() + ">";
        }

        void accept(Visitor &visitor) override {
            visitor.visitNamespace(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};


#endif