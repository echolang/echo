#ifndef GENERICVALUEEXPRNODE_H
#define GENERICVALUEEXPRNODE_H

#pragma once

#include "AST/ASTTypeParam.h"
#include "AST/ExprNode.h"
#include "Lexer.h"

namespace AST
{
    // **the value of a const generic parameter**, written as a bare identifier in an operand
    // position: `return N;` inside `struct sized<const usize N>`.
    //
    // it is not transient the way ConstRefExprNode is. a template body keeps it; CloneContext
    // replaces it with a literal when the parameter is bound, so an instantiated body sees an
    // ordinary integer and codegen never has to know about this node
    class GenericValueExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_generic_value);

        TokenReference token_name;
        const TypeParamDecl *param = nullptr;

        GenericValueExprNode(TokenReference token_name, const TypeParamDecl *param) :
            token_name(token_name),
            param(param)
        {};

        ~GenericValueExprNode() {};

        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_generic_value(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
