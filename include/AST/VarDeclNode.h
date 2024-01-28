#ifndef VARDECLNODE_H
#define VARDECLNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Lexer.h"

namespace AST 
{
    class VarDeclNode : public Node
    {
    public:
        TokenReference token_varname;
        TokenReference token_type;

        ValueType type;

        VarDeclNode(TokenReference token_varname, TokenReference token_type) : 
            token_varname(token_varname), token_type(token_type)
        {};

        ~VarDeclNode() {};

        static constexpr NodeType node_type = NodeType::n_vardecl;

        const std::string node_description() override {
            return "vardecl(" + token_type.value() + ">(" + token_varname.value() + ")";
        }

    private:

    };
};

#endif