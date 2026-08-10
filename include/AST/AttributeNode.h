#ifndef ATTRIBUTENODE_H
#define ATTRIBUTENODE_H

#pragma once

#include "AST/ASTAttributeValue.h"
#include "AST/ASTNode.h"
#include "Token.h"

#include <optional>

namespace AST
{
    class AttributeNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_attribute);

        TokenSlice attribute_tokens;
        TokenReference attribute_id;

        // what was written after the colon, or nothing for a flag like `#[inline]`.
        //
        // **a value, not an expression.** This used to be a NodeReferenceList holding whatever
        // Parser::parse_expr_ref made of the tokens, which put a live LiteralStringExprNode into the
        // module's arena for every `#[core: ...]` in the program and left every consumer to test node
        // types by hand. See AST::AttributeValue for why an expression is the wrong tool here
        std::optional<AttributeValue> value;

        AttributeNode(const TokenSlice &attribute_tokens, const TokenReference &attribute_id) :
            attribute_tokens(attribute_tokens),
            attribute_id(attribute_id)
        {
        };
        ~AttributeNode() {};

        const std::string node_description() override {
            if (!value.has_value()) {
                return "attr<" + attribute_id.value() + ">";
            }

            return "attr<" + attribute_id.value() + ": " + attribute_value_spelling(value.value()) + ">";
        }

        void accept(Visitor &visitor) override {
            visitor.visitAttribute(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };

    class AttributeList
    {
    public:
        AttributeList() {};
        ~AttributeList() {};

        void push_back(AttributeNode *attribute) {
            _attributes[attribute->attribute_id.value()].push_back(attribute);
        }

        // absent is not an error - asking whether a declaration carries an attribute is the
        // normal case, and every caller already null-checks. `at()` threw std::out_of_range
        // instead, which escaped as far as std::terminate for any bodyless function that did
        // not happen to carry an `intrinsic` attribute
        const std::vector<AttributeNode *> &get(const std::string &attribute_id) const {
            static const std::vector<AttributeNode *> none;
            auto it = _attributes.find(attribute_id);
            return it == _attributes.end() ? none : it->second;
        }

        AttributeNode *get_first(const std::string &attribute_id) const {
            auto it = _attributes.find(attribute_id);
            if (it == _attributes.end() || it->second.empty()) {
                return nullptr;
            }
            return it->second.front();
        }

    private:
        std::unordered_map<std::string, std::vector<AttributeNode *>> _attributes;

    };
};

#endif
