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
    // merely untidy, it is expensive and occasionally wrong: `Monomorphizer::snapshot_calls` mints a generic
    // instance per call it finds, `TypeLowering::build_function_maps`' second loop declares a symbol *and
    // queues a linkonce_odr body* per callee it finds, and `Monomorphizer::finalize_calls` will report an
    // unknown name against one. so a discarded `const if` arm still had its `operator []` emitted, and a
    // name it got wrong could still have failed the compile.
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
};

#endif
