#ifndef ASTNAMESPACE_H
#define ASTNAMESPACE_H

#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

#include "ASTDeclarationSite.h"
#include "ASTSymbol.h"

#define ECO_NAMESPACE_SEPARATOR "::"

namespace AST
{
    // a namespace, and - for a *lexical* one - the declaration scope of a `{ }` block.
    //
    // that a lexical scope is a namespace is the whole of B18's fix: `{ function helper() {} }` puts
    // helper in the block's lexical namespace, and every question about it is then already answered.
    // FunctionRegistry::overloads walks parent() outward and lets the innermost non-empty set win, so
    // an inner helper hides an outer one and an unrelated name still resolves outward.
    // find_by_signature searches one namespace, so two blocks each declaring helper(int32) do not
    // collide. FunctionCallExprNode::lookup_namespace already records where a call looked its name up
    // and already survives a clone, so the monomorphizer's fixpoint re-derives candidates with no new
    // field. And mangle_function_name already prefixes the namespace path, so the two helpers get
    // distinct symbols. four reuses, no new rule
    //
    // a lexical namespace is deliberately unspellable: it lives in `_lexical_children`, keyed by the
    // declaration site of its opening brace rather than by a name, so NamespaceManager::retrieve/get
    // cannot reach it and no `namespace <x>;` can merge into it
    class Namespace
    {
        friend class NamespaceManager;

    public:

        Namespace(const std::string &name) : _name(name), _display_name(name) {};
        ~Namespace() {};

        // disallow copy and move
        Namespace(const Namespace &) = delete;
        Namespace(Namespace &&) = delete;

        bool is_root() const {
            return _parent == nullptr;
        }

        // a block's declaration scope rather than something the user wrote
        bool is_lexical() const {
            return _is_lexical;
        }

        // the unique name - the one a symbol is built from. `display_name()` is what a human reads;
        // the two differ only for a lexical namespace, so every reader has to say which it means
        std::string name() const {
            return _name;
        }

        std::string display_name() const {
            return _display_name;
        }

        // path segments from the root down, excluding the root itself. the segments a human reads, so
        // a lexical block contributes the enclosing function's name and a diagnostic says
        // `outer::helper(int32)`. deliberately *not* the mangling path - see mangling_segments
        std::vector<std::string> path_segments() const;

        // the segments a symbol name is built from. identical to path_segments for everything the user
        // wrote, and unique per block for a lexical namespace - two sibling blocks both display as
        // `outer` but must not mangle alike, or two helper bodies land in one llvm::Function
        std::vector<std::string> mangling_segments() const;

        // fully qualified path, root first ("a::b"), empty for the root namespace
        std::string full_name() const;

        const Namespace *parent() const {
            return _parent;
        }

        // the nearest namespace the user could have written - itself, unless it is lexical. types live
        // here: a lexical scope holds function declarations only, so far
        const Namespace *declaring_namespace() const;
        Namespace *declaring_namespace();

        void push_symbol(std::unique_ptr<Symbol> symbol);

        std::string debug_dump_symbols() const;

    private:
        // the one walk both segment views are: root-exclusive, parent-first, reversed. `display` picks
        // `_display_name` over `_name` *and* drops an empty one, which is the only difference between
        // the two and exactly where they are allowed to differ
        std::vector<std::string> segments(bool display) const;

        Namespace *_parent = nullptr;

        // `_name` is unique among its siblings and is what a symbol name is built from;
        // `_display_name` is what a human reads. the two differ only for a lexical namespace, whose
        // name carries a discriminator its display name does not
        std::string _name;
        std::string _display_name;
        bool _is_lexical = false;

        std::unordered_map<std::string, std::unique_ptr<Namespace>> _children;

        // keyed by the block's opening brace, not by a name - which is what makes these unreachable
        // from any namespace path a user can write
        std::unordered_map<DeclarationSite, std::unique_ptr<Namespace>, DeclarationSiteHash> _lexical_children;

        std::unordered_map<std::string, std::unique_ptr<Symbol>> _symbols;
    };

    class NamespaceManager
    {
    public:
        NamespaceManager() : _root("") {};
        ~NamespaceManager() {};

        // returns the namespace for the given name, creating it if it doesn't exist
        Namespace &retrieve(const std::string &name);
        Namespace &retrieve(const std::vector<std::string> &parts);

        // the lexical namespace of the block whose opening brace is `site`, created on first ask.
        // create-or-reuse rather than create, because the declaration pass and the body pass both walk
        // the same braces and have to land on one object - the declaration site is what makes that
        // exact, the same way it reconciles a function declaration across those passes
        //
        // `display_name` is the enclosing function's name, which is what a diagnostic renders; the
        // namespace's own name gets a discriminator so two blocks of one function still mangle apart
        Namespace &retrieve_lexical(Namespace &parent, const DeclarationSite &site, const std::string &display_name);

        // returns the namespace for the given name, or nullptr if it doesn't exist
        const Namespace *get(const std::string &name) const;
        const Namespace *get(const std::vector<std::string> &parts) const;

        bool exists(const std::string &name) const;
        bool exists(const std::vector<std::string> &parts) const;

        // **the exact-namespace lookup**: `ns`'s own symbols and nothing else. what a *declaration*
        // site asks - "is this name already taken here" - and what an explicitly qualified
        // `geometry::Point` asks, where walking outward would silently answer with the root's `Point`
        Symbol *find_symbol(const std::string &fullname) const;
        Symbol *find_symbol(const std::string &symbol_name, const std::string &ns) const;
        Symbol *find_symbol(const std::string &symbol_name, const Namespace &ns) const;

        // **the scoped lookup**: `from` outward to the root, innermost wins. what a *use* site asks,
        // and the exact mirror of FunctionRegistry::overloads - a name written unqualified means the
        // nearest declaration of it, and an inner namespace hides an outer one rather than extending
        // it. types had only the exact lookup above, so a `namespace app;` file could not name a
        // root type at all, and a nested type's body - which parses inside a namespace named after
        // its owner - could name no declared type whatsoever
        Symbol *find_symbol_in_scope(const std::string &symbol_name, const Namespace &from) const;

        Namespace &root() { return _root; }

    private:
        Namespace _root;

        // makes every lexical namespace's `_name` unique in one step, without depending on how many
        // blocks a given parent happens to have seen. a plain counter is enough: the passes reuse an
        // existing lexical namespace rather than minting a second one, so each is numbered once, and
        // creation order is a fixed file order walked linearly - the same every run
        size_t _lexical_counter = 0;
    };
};

#endif