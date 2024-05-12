#ifndef SCOPENODE_H
#define SCOPENODE_H

#pragma once

#include "ASTNode.h"

#include <unordered_map>

namespace AST 
{
    class VarDeclNode;

    class ScopeNode : public Node
    {
        std::unordered_map<std::string, const VarDeclNode *> _declared_variables;

    public:
        ScopeNode *parent_ptr = nullptr;

        NodeReferenceList children;

        ScopeNode() {};
        ~ScopeNode() {};

        static constexpr NodeType node_type = NodeType::n_scope;

        const std::string node_description() override;

        void accept(Visitor& visitor) override {
            visitor.visitScope(*this);
        }

        inline ScopeNode &parent() const {
            assert(parent_ptr);
            return *parent_ptr;
        }

        inline bool is_root() const {
            return parent_ptr == nullptr;
        }

        inline bool is_leaf() const {
            return children.empty();
        }

        inline bool is_parent_of(const ScopeNode &node) const {
            for (const auto &child : node.children) {
                if (child.node() == this) {
                    return true;
                }
            }
            return false;
        }

        inline bool is_child_of(const ScopeNode &node) const {
            return parent_ptr == &node;
        }

        inline void add_child_scope(ScopeNode &child) {
            children.push_back(AST::make_ref(child));
            child.parent_ptr = this;
        }

        void add_vardecl(VarDeclNode &vardecl);

        bool is_varname_taken(const std::string &varname) const;

        const VarDeclNode *find_vardecl_by_name(const std::string &varname) const;

    private:

    };
};

#endif