#ifndef ASTFILEROOT_H
#define ASTFILEROOT_H

#pragma once

namespace AST
{
    class Node;
    class ScopeNode;

    // **the first child of a file root that is not a declaration**, or null when the file declares things
    // and does nothing.
    //
    // asked of a module that declares `#[target:]`s, where a file that is no target's entry is shared by
    // all of them: only an entry file's root becomes a program, so top-level code anywhere else would be
    // parsed, type-checked and then emitted nowhere. A *non-entry module's* file root has always been
    // dropped exactly that way and it is the wart the fixture in tests_eco/const/lib_const records - a
    // target module is the one place there is a manifest saying enough to refuse it instead.
    //
    // **an allow-list, not a deny-list**, and that direction is the content: a NodeType added without an
    // arm here counts as executable, so it is refused in a shared file rather than silently dropped from
    // one. The seven that pass are the shapes a file scope can hold that emit nothing on their own -
    // a function, a type, a constant, an attribute, a `use`, and the two namespace nodes. **A `$var` is not among
    // them**: a module-scope variable is storage plus an initializer that runs, which is the case that
    // reads most like a declaration and is not one.
    //
    // direct children only. A statement nested inside a declaration belongs to that declaration's body,
    // which is emitted from the walk over every file rather than from the entry file's root
    Node *first_top_level_statement(const ScopeNode &root);
};

#endif
