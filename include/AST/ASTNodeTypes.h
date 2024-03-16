#ifndef ASTNODETYPES_H
#define ASTNODETYPES_H

#pragma once

#include <type_traits>

namespace AST
{   
    class Node;

    // node type enum
    enum class NodeType {
        n_none,
        n_null,
        n_scope,
        n_literal,
        n_vardecl,
        n_type,
    };

    template<typename T>
    concept NodeTypeProvider = std::is_base_of_v<Node, T> && requires {
        { T::node_type } -> std::same_as<const NodeType&>;
    };
};

#endif