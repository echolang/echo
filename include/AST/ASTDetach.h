#ifndef ASTDETACH_H
#define ASTDETACH_H

#pragma once

#include <unordered_set>

namespace AST
{
    class Bundle;
    class Node;

    // **the arena stops answering for a subtree that has left the tree.**
    //
    // `NodeCollection::of_type` is not a tree walk - it hands back every node ever allocated of that type -
    // so a pass that replaces a statement leaves everything under the old one visible forever. that is not
    // merely untidy, it is expensive and occasionally wrong: `TypeLowering::build_function_maps`' second
    // loop declares a symbol *and queues a linkonce_odr body* per callee it finds in the arena. so a
    // discarded `const if` arm still had its `operator []` emitted if the forget was late.
    // `Monomorphizer::snapshot_calls` walks the live tree and does not need this for itself.
    //
    // one owner because three passes discard subtrees and the walk has to be **total** - it is handed
    // straight to `NodeCollection::forget`, and forgetting less than the whole of what went away leaves the
    // problem, while forgetting more is a symbol nothing declares.
    //
    // **the obligation on every caller**: release whatever you are *keeping* from `root` before calling
    // this. AST::ConstFolding::splice nulls the arm it selected first, and says why there
    void forget_subtree(Bundle &bundle, Node &root);

    // the collecting half, for a caller that discards more than one subtree before the arena has to be
    // told. `NodeCollection::forget` is an erase-remove over every bucket of every module, so one call per
    // subtree is one whole-arena sweep per subtree - and AST::ConstFolding discards one per `const if` and
    // one per `const(...)`, on every round of the monomorphizer's fixpoint. the same obligation applies
    void collect_subtree(Node &root, std::unordered_set<const Node *> &gone);

    // **a round's worth of discards, told to the arena once.**
    //
    // the pair above spelled out: the set, plus the flush that empties it. Both rewriting passes inside the
    // monomorphizer's fixpoint discard subtrees per round and both have to batch for the reason
    // `collect_subtree` gives - so "collect as you go, flush once when the round ends" is one object here
    // rather than a raw set and a two-line flush repeated in each of them.
    //
    // **the obligation on every caller is `forget_subtree`'s**: release whatever you are keeping from a
    // subtree before collecting it.
    class DetachBatch
    {
    public:

        void collect(Node &root) {
            collect_subtree(root, _gone);
        }

        // hands this round's discards over and starts the next one empty. `Bundle::forget_nodes` returns
        // immediately on an empty set, so a round that discarded nothing costs nothing
        void flush(Bundle &bundle);

    private:

        std::unordered_set<const Node *> _gone;
    };
};

#endif
