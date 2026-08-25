#ifndef TYPENODE_H
#define TYPENODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ASTValueType.h"
#include "Lexer.h"

#include <optional>
#include <vector>

namespace AST
{
    class TypeNode : public Node
    {
    public:

        const ValueType type;

        // the token at the start of the written type (`const`, `ptr`, `function`, …)
        // source_token_of and the monomorphizer's "the author wrote a type" bit both read this
        std::optional<TokenReference> type_token;

        // identifiers the author wrote in this spelling, each a TypeNode whose `type` is what
        // that identifier resolved to. parse_type fills them from parse_value_type; minted
        // nodes have none. owned so a later walk does not re-parse
        std::vector<TypeNode *> written_names;

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
