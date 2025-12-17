#ifndef ASTCLONE_H
#define ASTCLONE_H

#pragma once

#include <unordered_map>

#include "AST/ASTNode.h"
#include "AST/ASTNodeReference.h"
#include "AST/ASTValueType.h"

namespace AST
{
    // toolkit threaded through a deep clone of an AST subtree. It is the single place that
    // knows how to (a) emit new nodes into the right NodeCollection, (b) substitute types,
    // and (c) rebind references so that clones point at clones (not at the originals)
    //
    // every node's clone() records its old->new mapping *before* recursing into children,
    // so self-referential edges (e.g. a recursive function calling itself) resolve to the
    // in-progress clone instead of the original
    struct CloneContext
    {
        NodeCollection &nodes;              // where cloned nodes are emplaced (owner)
        const TypeSubstitution &subst;      // type-parameter declaration -> concrete type
        TypeRegistry &registry;             // interns generic applications during substitution

        std::unordered_map<const Node *, Node *> map;  // original -> clone

        CloneContext(NodeCollection &nodes, const TypeSubstitution &subst, TypeRegistry &registry)
            : nodes(nodes), subst(subst), registry(registry)
        {}

        // shallow copy-construct `from` into the collection and record the mapping. Callers
        // then deep-clone owned children / rebind cross-references / substitute types in place
        template <class T>
        T *shallow(const T *from) {
            T &copy = nodes.emplace_back<T>(*from);
            map[from] = &copy;
            return &copy;
        }

        // construct a fresh T from explicit constructor arguments (used where a shallow copy
        // won't do - e.g. TypeNode's type is const and must be set at construction). Records
        // the old->new mapping against `from`.
        template <class T, class... Args>
        T *make(const Node *from, Args &&...args) {
            T &node = nodes.emplace_back<T>(std::forward<Args>(args)...);
            map[from] = &node;
            return &node;
        }

        // deep-clone an owned child (dispatches through the virtual clone). Null-safe
        //
        // a node the map already holds is answered with *that* clone rather than cloned a second time.
        // two clones of one node inside one operation is never what a caller wants: for a declaration it
        // means two allocas and half the reads bound to each, and for any node it means the instance
        // carries a subtree the template does not. it is also what lets a clone() body clone a scope's
        // declarations ahead of the statements that read them - see ScopeNode::clone - without the child
        // loop that follows cloning them all over again
        template <class T>
        T *child(T *old) {
            if (!old) return nullptr;
            auto it = map.find(old);
            if (it != map.end()) return static_cast<T *>(it->second);
            return static_cast<T *>(old->clone(*this));
        }

        // resolve a cross-reference: the clone if the target was cloned in this subtree,
        // otherwise the original (shared). Null-safe
        template <class T>
        T *rebind(T *old) const {
            if (!old) return old;
            auto it = map.find(old);
            return it != map.end() ? static_cast<T *>(it->second) : old;
        }

        // deep-clone the node behind an owned NodeReference, preserving its type tag. answers with an
        // existing clone for the same reason `child` does
        NodeReference clone_ref(const NodeReference &ref) {
            if (!ref.has()) return make_void_ref();
            return NodeReference(ref.type(), child(ref.node()));
        }

        // rebind the node behind a cross-reference NodeReference (target not owned here)
        NodeReference rebind_ref(const NodeReference &ref) const {
            if (!ref.has()) return ref;
            auto it = map.find(ref.node());
            return it != map.end() ? NodeReference(ref.type(), it->second) : ref;
        }

        ValueType substitute(const ValueType &type) const {
            return substitute_type(type, subst, registry);
        }
    };
};

#endif
