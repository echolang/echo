#ifndef TYPENODE_H
#define TYPENODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "Lexer.h"

#include <optional>

namespace AST 
{
    class TypeNode : public Node
    {
    public:
        
        const ValueType type;

        std::optional<TokenReference> type_token;

        TypeNode(ValueType type, TokenReference type_token)
            : type(type), type_token(type_token)
        {};
        TypeNode(ValueType type)
            : type(type)
        {};
        ~TypeNode() {};

        ECO_AST_NODE_TYPE(n_type);

        // `type` is the single source of truth - it already renders its own const and pointer
        // levels, so prefixing them again here produced `type<const const int32>`
        const std::string node_description() override {
            return "type<" + type.get_type_desciption() + ">";
        }

        void accept(Visitor &visitor) override {
            visitor.visitType(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:

    };
};


#endif