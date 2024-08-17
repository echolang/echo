#ifndef VARREFNODE_H
#define VARREFNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Lexer.h"
#include "VarNode.h"
#include "VarMemberNode.h"
#include "ExprNode.h"

namespace AST 
{
    class VarRefNode : public ExprNode
    {
    public:
        static constexpr NodeType node_type = NodeType::n_varref;

        VarRefNode(VarNode *varnode) :
            _target_node(make_ref(varnode))
        {
        };

        VarRefNode(VarMemberNode *varnode) :
            _target_node(make_ref(varnode))
        {
        };

        ~VarRefNode() {};

        inline bool is_var() const {
            return _target_node.has_type<VarNode>();
        }

        inline bool is_varmember() const {
            return _target_node.has_type<VarMemberNode>();
        }

        ValueType result_type() const override;
        
        const std::string node_description() override {
            return "varref(" + _target_node.node()->node_description() + ")";
        }

        void accept(Visitor& visitor) override {
            visitor.visitVarRef(*this);
        }

    private:
        NodeReference _target_node;
        
    };
};

#endif