#ifndef ASTBUNDLE_H
#define ASTBUNDLE_H

#pragma once

#include "AST/ASTModule.h"
#include "AST/ASTCollector.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace AST
{
    // **one root's own build, inside a bundle that holds several.** a test run over several roots
    // compiles them as one program, and each root's tests still have to see the program their own
    // `echoc test` would have built: a table collected from "every declaration in the program" answers
    // differently once twenty-seven other roots are linked beside it. this is that narrower program -
    // which modules it is made of, and in what order its own build would have parsed them
    struct RootProgram
    {
        const Module *root = nullptr;

        // everything the root's own build compiles, the root included, in that build's parse order
        std::vector<const Module *> modules;

        // where `module` sits in this program's own order, or nullopt when the program does not hold it
        std::optional<size_t> position_of(const Module *module) const {
            const auto found = std::find(modules.begin(), modules.end(), module);
            if (found == modules.end()) {
                return std::nullopt;
            }
            return static_cast<size_t>(found - modules.begin());
        }

        // is a declaration written in `file` part of this program. a module the root depends on is
        // here as a library, so the files only its own test run adds are not
        bool holds(const File &file) const {
            if (file.module == nullptr || !position_of(file.module).has_value()) {
                return false;
            }
            return file.module == root || !file.tests_only;
        }
    };

    class Bundle
    {
    public:
        ModuleCollection modules;
        Collector collector;

        // one per root of a test run over several, in the order the roots were given. **empty for
        // every other build**, where the bundle is the one program and nothing needs a narrower view
        std::vector<RootProgram> root_programs;

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
