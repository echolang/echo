#ifndef ASTNODE_H
#define ASTNODE_H

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <typeindex>
#include <unordered_map>

#include "ASTNodeTypes.h"
#include "ASTNodeReference.h"
#include "ASTVisitor.h"

namespace AST
{
    struct CloneContext;

    class Node
    {
    public:
        virtual ~Node() {}

        virtual const std::string node_description() = 0;

        virtual bool is_assignable() {
            return false;
        }

        virtual void accept(Visitor &visitor) = 0;

        // deep-copies this node (and the subtree it owns) into cc's target NodeCollection,
        // substituting types through cc's TypeSubstitution and rebinding intra-subtree
        // references to their clones. Pure-virtual so the compiler forces every concrete
        // node to provide one — the exhaustiveness the monomorphizer relies on.
        // implemented in src/AST/ASTClone.cpp.
        virtual Node *clone(CloneContext &cc) const = 0;
    };

    typedef std::vector<std::unique_ptr<Node>> NodeList;


    // node collection is just a wrapper around a node list
    class NodeCollection
    {
        std::unique_ptr<NodeList> nodes = std::make_unique<NodeList>();

        std::unordered_map<std::type_index, std::vector<Node *>> node_map;
    public:

        // emplace back 
        template <typename T, typename... Args>
            requires NodeTypeProvider<T>
        inline T &emplace_back(Args&&... args) {
            auto node = std::make_unique<T>(std::forward<Args>(args)...);
            auto &node_ref = *node;
            nodes->push_back(std::move(node));

            // store a reference to the node in the node map for the type
            node_map[typeid(T)].push_back(&node_ref);

            return node_ref;
        }

        template <typename T>
            requires NodeTypeProvider<T>
        inline const std::vector<T *> &of_type() const {
            static const std::vector<T *> empty;
            auto it = node_map.find(std::type_index(typeid(T)));
            if (it != node_map.end()) {
                return reinterpret_cast<const std::vector<T *> &>(it->second);
            }
            return empty;
        }

        inline size_t size() const {
            return nodes->size();
        }
               
    };
};

#endif