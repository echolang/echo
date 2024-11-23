#ifndef NULLNODE_H
#define NULLNODE_H

#pragma once

#include "ASTNode.h"
#include "ExprNode.h"

namespace AST
{
    // the null address
    //
    // null has no type of its own; it takes the type of whatever it is assigned to or compared
    // against, which the parser supplies from the expected type at the position it appears in
    // left unbound it stays a nullable pointer to nothing, and the type checker reports it
    class NullNode : public ExprNode
    {
    public:
        NullNode() {};
        NullNode(TokenReference token_null) : token_null(token_null) {};
        ~NullNode() {};

        ECO_AST_NODE_TYPE(n_null);

        std::optional<TokenReference> token_null;

        // the pointer type this null was given by its context, if any
        std::optional<ValueType> bound_type;

        ValueType result_type() const override {
            return bound_type.has_value() ? bound_type.value() : ValueType::make_unknown();
        }

        bool is_bound() const {
            return bound_type.has_value();
        }

        const std::string node_description() override {
            return "null<" + result_type().get_type_desciption() + ">";
        }

        void accept(Visitor &visitor) override {
            visitor.visitNull(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:

    };
};


#endif
