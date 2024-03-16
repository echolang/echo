#ifndef TYPENODE_H
#define TYPENODE_H

#pragma once

#include "ASTNode.h"
#include "../Lexer.h"

namespace AST 
{
    class TypeNode : public Node
    {
    public:
        TokenReference literal_token;

        TypeNode(TokenReference literal_token) : 
            literal_token(literal_token)
        {};
        ~TypeNode() {};

        static constexpr NodeType node_type = NodeType::n_type;

        const std::string node_description() override {
            return "type<" + literal_token.value() + ">";
        }

    private:

    };
};


#endif