#ifndef ATTRIBUTENODE_H
#define ATTRIBUTENODE_H

#pragma once

#include "AST/ASTNode.h"
#include "Token.h"

namespace AST 
{
    class ExprNode;

    class AttributeNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_attribute);
        
        TokenSlice attribute_tokens;
        TokenReference attribute_id;
        TokenList attribute_values;

        NodeReferenceList attribute_exprs;

        AttributeNode(const TokenSlice &attribute_tokens, const TokenReference &attribute_id) : 
            attribute_tokens(attribute_tokens), 
            attribute_id(attribute_id),
            attribute_values(TokenList(attribute_tokens.tokens))
        {
        };
        ~AttributeNode() {};

        const std::string node_description() override { 
            return "attr<" + attribute_id.value() + ">";
        }

        void accept(Visitor& visitor) override {
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