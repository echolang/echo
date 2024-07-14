#ifndef TYPENODE_H
#define TYPENODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Lexer.h"

#include <optional>

namespace AST 
{
    class TypeNode : public Node
    {
    public:
        
        const ValueType type;

        std::optional<TokenReference> type_token;

        bool is_const = false;

        bool is_pointer = false;

        TypeNode(ValueType type, TokenReference type_token)
            : type(type), type_token(type_token)
        {};
        TypeNode(ValueType type)
            : type(type)
        {};
        ~TypeNode() {};

        static constexpr NodeType node_type = NodeType::n_type;

        const std::string node_description() override {
            std::string const_str = is_const ? "const " : "";
            std::string prefix = is_pointer ? "ptr" : "type";
            return prefix + "<" + const_str + type.get_type_desciption() + ">";
        }

        void accept(Visitor& visitor) override {
            visitor.visitType(*this);
        }

    private:

    };
};


#endif