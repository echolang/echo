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
        n_vardecl,
        n_varref,
        n_type,
        n_expression,
    };

    template<typename T>
    concept NodeTypeProvider = std::is_base_of_v<Node, T> && requires {
        { T::node_type } -> std::same_as<const NodeType&>;
    };
};

#endif