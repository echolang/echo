#ifndef VARDECLNODE_H
#define VARDECLNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Lexer.h"
#include "TypeNode.h"

namespace AST 
{
    class VarDeclNode : public Node
    {
    public:
        TokenReference token_varname;

        TypeNode *type_n;

        VarDeclNode(TokenReference token_varname, TypeNode *type) : 
            token_varname(token_varname), type_n(type)
        {};

        ~VarDeclNode() {};

        static constexpr NodeType node_type = NodeType::n_vardecl;

        const std::string node_description() override {
            return "vardecl<" + type_n->node_description() + ">(" + token_varname.value() + ")";
        }

    private:

    };
};

#endif