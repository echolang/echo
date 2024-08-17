#ifndef MEMBERMUTNODE_H
#define MEMBERMUTNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Lexer.h"

namespace AST 
{
    class ExprNode;
    class MemberAccessNode;

    class MemberMutNode : public Node
    {
    public:
        static constexpr NodeType node_type = NodeType::n_membermut;

        MemberAccessNode *member_access;
        ExprNode *value_expr;

        MemberMutNode(MemberAccessNode *member_access, ExprNode *value_expr)
            : member_access(member_access), value_expr(value_expr)
        {
            assert(member_access != nullptr && "Member access cannot be null");
            assert(value_expr != nullptr && "Value expression cannot be null");
        };

        ~MemberMutNode() {};

        const std::string node_description() override;

        void accept(Visitor& visitor) override {
            visitor.visitMemberMut(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
}

#endif
