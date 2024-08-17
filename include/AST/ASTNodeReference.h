#ifndef ASTREFERENCE_H
#define ASTREFERENCE_H

#pragma once

#include <assert.h>
#include <vector>
#include "ASTNodeTypes.h"

namespace AST 
{
    class Node;

    class NodeReference
    {
        Node *parent_ptr = nullptr;
        NodeType parent_type = NodeType::n_void;

    public:
        NodeReference() = default;
        NodeReference(NodeType type, Node *ptr) : 
            parent_ptr(ptr), parent_type(type) 
        {}

        ~NodeReference() {}

        inline Node *node() const {
            return parent_ptr;
        }

        inline NodeType type() const {
            return parent_type;
        }

        inline bool has() const {
            return parent_ptr != nullptr;
        }
        
        template <typename T>
            requires NodeTypeProvider<T>
        inline bool has_type() const {
            return parent_ptr != nullptr && parent_type == T::node_type;
        }

        template <typename T>
            requires NodeTypeProvider<T>
        inline T &get() const {
            assert(has_type<T>());
            return *static_cast<T*>(parent_ptr);
        }

        template <typename T>
            requires NodeTypeProvider<T>
        inline T *get_ptr() const {
            assert(has_type<T>());
            return static_cast<T*>(parent_ptr);
        }

        inline bool is_expression_node() const {
            assert(has());
            return parent_type == NodeType::n_expr_binary ||
                   parent_type == NodeType::n_expr_unary ||
                   parent_type == NodeType::n_expr_call ||
                   parent_type == NodeType::n_varref ||
                   parent_type == NodeType::n_expr_varptr ||
                   parent_type == NodeType::n_expr_void ||
                   parent_type == NodeType::n_literal_float ||
                   parent_type == NodeType::n_literal_int ||
                   parent_type == NodeType::n_literal_bool ||
                   parent_type == NodeType::n_literal_string;
        }

        template <typename T>
        inline T *unsafe_ptr() const {
            return static_cast<T*>(parent_ptr);
        }
    };

    typedef std::vector<NodeReference> NodeReferenceList;
    
    template <NodeTypeProvider T>
    const NodeReference make_ref(T *node) {
        static_assert(std::is_base_of_v<Node, T>, "T must be derived from Node");
        assert(T::node_type == node->node_type);
        return NodeReference(T::node_type, static_cast<Node*>(node));
    }

    template <NodeTypeProvider T>
    const NodeReference make_ref(T &node) {
        static_assert(std::is_base_of_v<Node, T>, "T must be derived from Node");
        assert(T::node_type == node.node_type);
        return NodeReference(T::node_type, static_cast<Node*>(&node));
    }

    inline const NodeReference make_void_ref() {
        return NodeReference(NodeType::n_void, nullptr);
    }
};

#endif