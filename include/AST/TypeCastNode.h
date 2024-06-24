#ifndef TYPECASTNODE_H
#define TYPECASTNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Lexer.h"

#include "ExprNode.h"

namespace AST 
{
    class TypeCastNode : public ExprNode
    {
    public:
        static constexpr NodeType node_type = NodeType::n_type_cast;
        
        ValueType cast_to;
        ExprNode* expr;

        TypeCastNode(ValueType cast_to, ExprNode* expr, bool implicit = false) : 
            ExprNode(implicit),
            cast_to(cast_to), 
            expr(expr)
        {}

        ValueType result_type() const override {
            return cast_to;
        }

        const std::string node_description() override {
            return "cast<" + cast_to.get_type_desciption() + ">(" + expr->node_description() + ")";
        }
        

        void accept(Visitor& visitor) override {
            visitor.visitTypeCast(*this);
        }

    private:

    };
};
#endif