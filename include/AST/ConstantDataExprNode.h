#ifndef CONSTANTDATAEXPRNODE_H
#define CONSTANTDATAEXPRNODE_H

#pragma once

#include "AST/ExprNode.h"

#include <cstdint>
#include <vector>

namespace AST
{
    // a long table of folded integers as `ptr<const E>`, in a private global. minted by
    // CollectionLiteralExpansion so the table is one copy; never written, no owned edges.
    // addressless: it already is an address
    class ConstantDataExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_constant_data);

        TokenReference token_bracket;

        // integer lanes only
        ValueType element_type = ValueType::make_unknown();

        // ConstFold's bits, one word per element
        std::vector<uint64_t> bits;

        ConstantDataExprNode(TokenReference token_bracket, ValueType element_type, std::vector<uint64_t> bits)
            : token_bracket(token_bracket), element_type(std::move(element_type)), bits(std::move(bits))
        {}

        ~ConstantDataExprNode() {}

        ValueType result_type() const override {
            return ValueType::make_pointer(ValueType::make_const(element_type), true);
        }

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_constant_data_expr(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
