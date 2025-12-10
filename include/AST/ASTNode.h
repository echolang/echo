#ifndef ASTNODE_H
#define ASTNODE_H

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

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

        // runtime dual of the per-class `static constexpr node_type`; lets a bare Node*
        // rejoin the NodeType-tag idiom (make_ref/has_type<T>) without RTTI. pure-virtual
        // for the same exhaustiveness reason as accept()/clone()
        virtual NodeType get_node_type() const = 0;

        // deep-copies this node (and the subtree it owns) into cc's target NodeCollection,
        // substituting types through cc's TypeSubstitution and rebinding intra-subtree
        // references to their clones. Pure-virtual so the compiler forces every concrete
        // node to provide one - the exhaustiveness the monomorphizer relies on
        // implemented in src/AST/ASTClone.cpp
        virtual Node *clone(CloneContext &cc) const = 0;
    };

    // re-tag a bare Node* into a NodeReference using its runtime kind, so a raw pointer
    // can rejoin the has_type<T>()/get_ptr<T>() idiom instead of reaching for RTTI. defined
    // here (not in ASTNodeReference.h) because it needs the complete Node type
    inline const NodeReference make_ref(Node *node) {
        return node ? NodeReference(node->get_node_type(), node) : make_void_ref();
    }

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

        // **stop answering for these nodes.** the unique_ptrs stay in `nodes`, so every pointer anything
        // still holds stays valid - what changes is only what `of_type` reports.
        //
        // it exists because that answer was wrong for a subtree no scope holds any more, and because the
        // sweeps reading it are the expensive ones. `of_type` is not a tree walk, so a pass that replaces
        // a statement leaves everything under the old one visible forever: Monomorphizer::snapshot_calls
        // mints a generic instance per call it finds, TypeLowering::build_function_maps' second loop
        // declares a symbol and **queues a linkonce_odr body** per callee it finds, and
        // Monomorphizer::finalize_calls will happily report an unknown name against one. so an arm
        // AST::ConstFolding discarded still had its callees emitted, and could still fail the compile.
        //
        // **order-preserving, and that is load-bearing**: `of_type` order is insertion order, and it decides
        // the order codegen declares functions in - IR goldens name symbols by position - so this is an
        // erase-remove and never a swap-and-pop.
        //
        // **the obligation this creates**: forgetting a node the tree still holds makes a live call
        // invisible, which is a missing symbol at link time. so a caller must walk exactly what is going
        // away, which is why the passes that discard release each subtree they keep *before* collecting
        // **only the buckets these nodes are actually in.** the sweep is an erase-remove over a bucket
        // and the arena has one per node kind ever allocated, so touching them all costs a pass over
        // every node in the program - and both rewriting passes inside the monomorphizer's fixpoint
        // flush a batch per round, to remove a handful of nodes of five or six kinds.
        //
        // the *dynamic* type is the key, and it is the same one the node was filed under: emplace_back
        // requires NodeTypeProvider<T> and constructs a `T`, so every bucket is keyed by a concrete
        // leaf kind and `typeid(*node)` cannot name a different one. that identity is what keeps this
        // total - under-collecting here is the silent direction, a live call left visible after its
        // statement went away
        static std::unordered_set<std::type_index> kinds_of(const std::unordered_set<const Node *> &gone) {
            std::unordered_set<std::type_index> kinds;

            for (const Node *node : gone) {
                if (node != nullptr) {
                    kinds.insert(std::type_index(typeid(*node)));
                }
            }

            return kinds;
        }

        void forget(const std::unordered_set<const Node *> &gone) {
            forget(gone, kinds_of(gone));
        }

        // the same, for a caller sweeping every module of a bundle: the set of kinds is a property of
        // `gone` and not of the arena, so deriving it once and handing it down is one `typeid` pass
        // instead of one per module
        void forget(
            const std::unordered_set<const Node *> &gone,
            const std::unordered_set<std::type_index> &kinds) {
            if (gone.empty()) {
                return;
            }

            for (const auto &kind : kinds) {
                const auto found = node_map.find(kind);

                if (found == node_map.end()) {
                    continue;
                }

                auto &bucket = found->second;

                bucket.erase(
                    std::remove_if(bucket.begin(), bucket.end(),
                        [&gone](Node *node) { return gone.count(node) > 0; }),
                    bucket.end());
            }
        }
    };
};

#endif