#ifndef ASTNODETYPES_H
#define ASTNODETYPES_H

#pragma once

#include <concepts>
#include <type_traits>

namespace AST
{   
    class Node;

    // node type enum
    enum class NodeType
    {
        n_void,
        n_null,
        n_scope,
        n_operator,
        n_literal,
        n_literal_float,
        n_literal_int,
        n_literal_bool,
        n_literal_string,
        n_vardecl,
        n_var,
        n_varref,
        n_assign,
        n_type,
        n_type_cast,
        n_expr_binary,
        n_expr_unary,
        n_expr_call,
        n_expr_addrof,
        n_expr_deref,
        n_expr_peel,
        n_expr_move,
        n_expr_index,
        n_expr_array_literal,
        n_expr_void,
        n_expr_class_alloc,
        n_expr_retain,
        n_expr_strong,
        n_expr_null_coalesce,
        n_expr_optional_chain,
        n_expr_chain_base,
        n_expr_closure,
        n_expr_indirect_call,
        n_expr_instanceof,
        n_expr_temp_bind,
        n_release,
        n_func_decl,
        n_func_return,
        n_if_statement,
        n_guard,
        n_while_statement,
        n_for_statement,
        n_loop_control,
        n_foreach,
        n_namespace_decl,
        n_namespace,
        n_attribute,
        n_type_decl,
        n_member_access,
    };

    template<typename T>
    concept NodeTypeProvider = std::is_base_of_v<Node, T> && requires {
        { T::node_type } -> std::same_as<const NodeType&>;
    };
};

// declares a node's compile-time NodeType tag together with the matching runtime accessor
// (get_node_type), so a bare Node* can rejoin the has_type<T>() idiom without RTTI. use inside
// the public section of every concrete AST::Node subclass, e.g. ECO_AST_NODE_TYPE(n_varref)
#define ECO_AST_NODE_TYPE(kind) \
    static constexpr AST::NodeType node_type = AST::NodeType::kind; \
    AST::NodeType get_node_type() const override { return node_type; }

#endif