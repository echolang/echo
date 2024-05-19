#ifndef VARREFNODE_H
#define VARREFNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Lexer.h"
#include "VarDeclNode.h"

namespace AST 
{
    class VarRefNode : public Node
    {
    public:
        TokenReference token_varname;
        VarDeclNode *decl;

        VarRefNode(TokenReference token_varname, VarDeclNode *decl) :
            token_varname(token_varname), decl(decl)
        {
            assert(decl != nullptr && "VarRefNode: decl is null");
        };

        ~VarRefNode() {};

        static constexpr NodeType node_type = NodeType::n_varref;
        
        const std::string node_description() override {

            return "varref<"+ decl->type_node()->node_description() +">(" + decl->name_full() + ")";
        }

        void accept(Visitor& visitor) override {
            visitor.visitVarRef(*this);
        }

    private:

    };
};

#endif