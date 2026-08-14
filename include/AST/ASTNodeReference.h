#ifndef ASTNODEREFERENCE_H
#define ASTNODEREFERENCE_H

#pragma once

#include <assert.h>
#include <vector>
#include "AST/ASTNodeTypes.h"

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

        // every ExprNode subclass must be listed here. a missing entry is silent: the node still
        // parses and lowers, it just stops being recognised as an expression, so it drops out of
        // implicit conversion (try_implicit_cast) and out of anything else that gates on this
        // n_member_access was missing, which is why `$s->x * 3.14` never got its implicit cast
        inline bool is_expression_node() const {
            assert(has());
            return parent_type == NodeType::n_expr_binary ||
                   parent_type == NodeType::n_expr_unary ||
                   parent_type == NodeType::n_expr_call ||
                   parent_type == NodeType::n_varref ||
                   parent_type == NodeType::n_expr_addrof ||
                   parent_type == NodeType::n_expr_deref ||
                   parent_type == NodeType::n_expr_peel ||
                   parent_type == NodeType::n_expr_move ||
                   parent_type == NodeType::n_expr_index ||
                   parent_type == NodeType::n_expr_array_literal ||
                   parent_type == NodeType::n_expr_void ||
                   parent_type == NodeType::n_expr_class_alloc ||
                   parent_type == NodeType::n_expr_retain ||
                   parent_type == NodeType::n_expr_strong ||
                   parent_type == NodeType::n_expr_null_coalesce ||
                   parent_type == NodeType::n_expr_optional_chain ||
                   parent_type == NodeType::n_expr_chain_base ||
                   parent_type == NodeType::n_expr_closure ||
                   parent_type == NodeType::n_expr_indirect_call ||
                   parent_type == NodeType::n_expr_instanceof ||
                   parent_type == NodeType::n_expr_temp_bind ||
                   parent_type == NodeType::n_expr_match ||
                   parent_type == NodeType::n_expr_const_ref ||
                   parent_type == NodeType::n_expr_const ||
                   parent_type == NodeType::n_member_access ||
                   parent_type == NodeType::n_literal_float ||
                   parent_type == NodeType::n_literal_int ||
                   parent_type == NodeType::n_literal_bool ||
                   parent_type == NodeType::n_literal_string ||
                   parent_type == NodeType::n_string_interpolation ||
                   parent_type == NodeType::n_expr_static_property;
            // **n_const_if is deliberately absent**, beside n_scope, n_if_statement and n_foreach: a
            // `const if` is a statement. its sibling n_expr_const *is* here, because `const(...)` is an
            // expression - the two spellings share a keyword and nothing else
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
