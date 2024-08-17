#ifndef MEMBERACCESSNODE_H
#define MEMBERACCESSNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTNodeReference.h"
#include "ExprNode.h"
#include "../Token.h"

namespace AST 
{
    class MemberAccessNode : public ExprNode
    {
    public:
        static constexpr NodeType node_type = NodeType::n_member_access;
        
        MemberAccessNode(NodeReference base, TokenReference member_name);
        ~MemberAccessNode() {}

        const std::string node_description() override {
            return _base_node.node()->node_description() + " -> " + _member_name.value();
        }

        void accept(Visitor& visitor) override {
            visitor.visitMemberAccess(*this);
        }

        ValueType result_type() const override;
    private: 
        NodeReference _base_node;
        TokenReference _member_name;
        size_t _member_index;
    };
};

#endif