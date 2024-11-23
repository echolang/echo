#ifndef VARREFNODE_H
#define VARREFNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "Lexer.h"
#include "VarNode.h"
#include "ExprNode.h"

namespace AST 
{
    class VarRefNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_varref);

        VarRefNode(VarNode *varnode) :
            _target_node(make_ref(varnode))
        {
        };

        ~VarRefNode() {};

        inline bool is_var() const {
            return _target_node.has_type<VarNode>();
        }

        inline VarNode &get_var() const {
            return _target_node.get<VarNode>();
        }

        ValueType result_type() const override;
        
        const std::string node_description() override {
            // return "varref(" + _target_node.node()->node_description() + ")";
            return "varref<" + result_type().get_type_desciption() + ">(" + _target_node.node()->node_description() + ")";
        }

        void accept(Visitor &visitor) override {
            visitor.visitVarRef(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:
        NodeReference _target_node;
        
    };
};

#endif