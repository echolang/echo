#ifndef ASTNODETYPES_H
#define ASTNODETYPES_H

#pragma once

#include <type_traits>

namespace AST
{   
    class Node;

    // node type enum
    enum class NodeType {
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
        n_varref,
        n_varmut,
        n_type,
        n_type_cast,
        n_expr_binary,
        n_expr_unary,
        n_expr_call,
        n_expr_varref,
        n_expr_varptr,
        n_expr_void,
        n_func_decl,
        n_func_return,
        n_if_statement,
        n_while_statement,
        n_for_statement,
        n_namespace_decl,
        n_namespace,
        n_attribute
    };

    template<typename T>
    concept NodeTypeProvider = std::is_base_of_v<Node, T> && requires {
        { T::node_type } -> std::same_as<const NodeType&>;
    };
};

#endif