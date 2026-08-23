#ifndef ASTTEST_H
#define ASTTEST_H

#pragma once

#include <string>

namespace AST
{
    class FunctionDeclNode;

    // the name a test declaration's *virtual* name token carries - `"test$adds_up"` for `test adds_up { }`.
    //
    // the `$` is the whole of it: no identifier can hold one, so a test is **unspellable**, and nothing
    // needs a rule forbidding a call to it. AST::operator_function_name does the same thing for the same
    // reason, and a test needs it more: an operator is at least in an overload set, while a test is in none.
    //
    // a `$` rather than the space an operator's decoration uses, because this name reaches the mangler
    // untouched and a symbol carrying a space is unportable at best. A closure's `closure$N` is the
    // precedent, and it holds for the same reason: unspellable in Echo, ordinary in an object file
    std::string test_function_name(const std::string &name);

    // one `test <name> { ... }`, as the driver reads it back.
    //
    // **the record, and not a second source of truth.** Everything here except `group` and
    // `expects_death` is derivable from `decl` - and both of those are on the declaration's
    // attributes - so this exists to be *enumerable*: codegen and the semantic passes find a test by
    // walking the node arena like any other function, and a test runner has to find them by module
    // and by file. Kept on AST::Module rather than in a registry, because a test is not a symbol and
    // nothing ever looks one up by name.
    //
    // **the file is not here**, deliberately: it is `decl->declared_in`, the one record of where a
    // declaration was written, and a copy beside it would be the fourth answer AST::DeclarationOrigin
    // exists to stop there being
    struct TestDeclaration
    {
        // as written. **unique per file and deliberately not per module**: two files may each declare a
        // `test adds_up`, which is what makes a file's tests readable without prefixing every one of them
        std::string name;

        // `#[group: "..."]`, or empty when the author wrote none. A tag for a runner to select on and
        // nothing more - the compiler never reads it
        std::string group;

        FunctionDeclNode *decl = nullptr;

        // `#[tests: expects death]`. inverts the runner's pass condition: the child must exit
        // non-zero. trailing so existing aggregate inits keep compiling
        bool expects_death = false;
    };
};

#endif
