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
        static constexpr NodeType node_type = NodeType::n_attribute;
        
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
    };

    class AttributeList
    {
    public:
        AttributeList() {};
        ~AttributeList() {};

        void push_back(AttributeNode *attribute) {
            _attributes[attribute->attribute_id.value()].push_back(attribute);
        }

        const std::vector<AttributeNode *> &get(const std::string &attribute_id) const {
            return _attributes.at(attribute_id);
        }

        AttributeNode *get_first(const std::string &attribute_id) const {
            return _attributes.at(attribute_id).front();
        }

    private:
        std::unordered_map<std::string, std::vector<AttributeNode *>> _attributes;
        
    };
};

#endif