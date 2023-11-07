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
        NodeType parent_type = NodeType::n_none;

    public:
        NodeReference() = default;
        NodeReference(NodeType type, Node *ptr) : 
            parent_ptr(ptr), parent_type(type) 
        {}

        ~NodeReference() {}

        inline bool has() { 
            return parent_ptr != nullptr; 
        }
        
        template <typename T>
            requires NodeTypeProvider<T>
        inline bool has_type() const {
            return parent_ptr != nullptr && parent_type == T::node_type;
        }
    };

    typedef std::vector<NodeReference> NodeReferenceList;
    
    template <NodeTypeProvider T>
    const NodeReference make_ref(T *node) {
        static_assert(std::is_base_of_v<Node, T>, "T must be derived from Node");
        assert(T::node_type == node->node_type);
        return NodeReference(T::node_type, static_cast<Node*>(node));
    }
};

#endif