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

        // the identifier as the author wrote it, including a namespace prefix. empty means
        // type_token's spelling. written_names leaves use this so TypeChecker can say
        // `Unknown type 'foo::Nope'` from a node that only holds the last token
        std::string written_spelling;

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

    // **the placeholder a borrow binding is declared with.** a member call addresses its receiver
    // unless the receiver is already an address, and the parser decides that from the type. MatchParser
    // and ForeachParser plant this; the lowering replaces the pointee once V is known. a by-value
    // binding stays untyped: a method on that `$m` *should* take `&$m`
    inline ValueType untyped_borrow_type() {
        return ValueType::make_pointer(ValueType::make_unknown(), false);
    }

    // **a written type name that never resolved.** the parser is silent (a miss in the declaration
    // pass can be a not-yet, and parse_type is called speculatively), so TypeChecker::visitType
    // asks this of every TypeNode RecursiveVisitor reaches through type_edge. a leaf whose type
    // is unknown is the name; minted nodes have no leaves and cannot be mistaken for a miss
    struct UnresolvedTypeName
    {
        std::string sentence;
        TokenReference at;

        UnresolvedTypeName(std::string sentence, TokenReference at)
            : sentence(std::move(sentence)), at(at)
        {}
    };

    inline std::optional<UnresolvedTypeName> unresolved_type_name_refusal(const TypeNode &node)
    {
        for (const TypeNode *leaf : node.written_names) {
            if (leaf == nullptr || !leaf->type.is_unknown() || !leaf->type_token.has_value()) {
                continue;
            }

            const std::string &name = leaf->written_spelling.empty()
                ? leaf->type_token->value()
                : leaf->written_spelling;

            return UnresolvedTypeName(
                "Unknown type '" + name + "'",
                leaf->type_token.value());
        }

        return std::nullopt;
    }
};


#endif
