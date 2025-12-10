#ifndef ASTBUNDLE_H
#define ASTBUNDLE_H

#pragma once

#include "ASTModule.h"
#include "ASTCollector.h"

namespace AST
{  
    class Bundle
    {
    public:
        ModuleCollection modules;
        Collector collector;

        Bundle() {};
        ~Bundle() {};

        // **the bundle-wide half of NodeCollection::forget.** a pass that discards a subtree knows which
        // nodes went away and should not also have to know which module's arena owns each one - a clone the
        // monomorphizer appended lives in the *template's* module rather than in the one being walked, so
        // "the current module" is not a reliable answer. asking every collection is, and this is a rare
        // operation, so paying for that is cheaper than a second answer to which arena holds a node
        void forget_nodes(const std::unordered_set<const Node *> &gone) {
            if (gone.empty()) {
                return;
            }

            // which node kinds went away is a fact about `gone`, so it is derived once here rather than
            // per module - the sweep below is otherwise one `typeid` pass over the whole batch per module,
            // and both rewriting passes inside the fixpoint flush a batch every round
            const auto kinds = NodeCollection::kinds_of(gone);

            for (auto &module_ptr : modules) {
                module_ptr->nodes.forget(gone, kinds);
            }
        }

    private:

    };
};

#endif