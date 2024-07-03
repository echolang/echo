#ifndef VARMUTNODE_H
#define VARMUTNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Lexer.h"
#include "TypeNode.h"

namespace AST 
{
    class ExprNode;
    class VarDeclNode;

    class VarMutNode : public Node
    {
    public:
    
        TokenReference token_varname;

        ExprNode *value_expr;
        VarDeclNode *var_decl = nullptr;

        VarMutNode(TokenReference token_varname, ExprNode *value_expr, VarDeclNode *var_decl)
            : token_varname(token_varname), value_expr(value_expr), var_decl(var_decl)
        {};
 

        ~VarMutNode() {};

        static constexpr NodeType node_type = NodeType::n_varmut;

        const std::string &name_full() const {
            return token_varname.value();
        }

        const std::string node_description() override;

        void accept(Visitor& visitor) override {
            visitor.visitVarMut(*this);
        }

    private:

    };
};

#endif