#ifndef ASTNODE_H
#define ASTNODE_H

#pragma once

#include <string>
#include <vector>
#include <memory>
#include "ASTNodeTypes.h"
#include "ASTNodeReference.h"

namespace AST {
    class Node
    {
    public:
        virtual ~Node() {}

        virtual const std::string node_description() = 0;

        virtual const bool is_assignable() {
            return false;
        }
    };

    typedef std::vector<std::unique_ptr<Node>> NodeList;


    // node collection is just a wrapper around a node list
    class NodeCollection
    {
        std::unique_ptr<NodeList> nodes = std::make_unique<NodeList>();
    public:

        // emplace back 
        template <typename T, typename... Args>
            requires NodeTypeProvider<T>
        inline T &emplace_back(Args&&... args) {
            auto node = std::make_unique<T>(std::forward<Args>(args)...);
            auto &node_ref = *node;
            nodes->push_back(std::move(node));
            return node_ref;
        }

        
    };
};

#endif