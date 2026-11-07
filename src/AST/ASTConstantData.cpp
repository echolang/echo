#include "AST/ASTConstantData.h"

#include "AST/ASTConstFold.h"
#include "AST/ASTLiteralTyping.h"
#include "AST/ASTNode.h"
#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"

std::optional<std::vector<uint64_t>> AST::fold_integer_table(
    ArrayLiteralExprNode &array,
    const ValueType &element,
    NodeCollection &nodes
)
{
    if (!element.is_integer_type() || element.is_const()) {
        return std::nullopt;
    }

    std::vector<uint64_t> bits;
    bits.reserve(array.elements.size());

    for (ExprNode *&written : array.elements) {
        const LiteralTyping typing = type_literal_at(written, element, nodes);

        if (typing.result == LiteralTyping::Result::t_refused) {
            return std::nullopt;
        }

        if (typing.result == LiteralTyping::Result::t_typed) {
            written = typing.node;
        }

        const ConstFoldResult folded = const_fold(written);

        if (!folded.is_folded() || ValueType::make_mutable(folded.type) != element) {
            return std::nullopt;
        }

        bits.push_back(folded.bits);
    }

    return bits;
}
