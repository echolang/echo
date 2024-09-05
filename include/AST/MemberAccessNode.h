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
        ECO_AST_NODE_TYPE(n_member_access);
        
        MemberAccessNode(NodeReference base, TokenReference member_name);
        ~MemberAccessNode() {}

        const std::string node_description() override {
            return "ma<" + result_type().get_type_desciption() + ">(" + _base_node.node()->node_description() + "->" + _member_name.value() + ")";
        }

        void accept(Visitor& visitor) override {
            visitor.visitMemberAccess(*this);
        }

        Node *clone(CloneContext &cc) const override;

        ValueType result_type() const override;

        // the struct the base ultimately addresses, reached through every pointer level. unknown
        // when there is no base or it is not an expression - callers read that as "cannot tell",
        // which is why this does not collapse to void: the type checker still has to distinguish
        // "not a struct" from "a class" (todo/A1)
        ValueType base_target_type() const;

        inline NodeReference &get_base_node() const {
            return const_cast<NodeReference&>(_base_node); 
        }
        
        inline TokenReference& get_member_name() const { 
            return const_cast<TokenReference&>(_member_name); 
        }
        
    private: 
        NodeReference _base_node;
        TokenReference _member_name;
    };
};

#endif