#ifndef ASTBUILTIN_H
#define ASTBUILTIN_H

#pragma once

#include <optional>
#include <string>

namespace AST
{
    // the compiler builtins a function declaration can be bound to with `#[builtin: "..."]`
    //
    // a builtin is answered by the compiler at the call site rather than being emitted as a
    // function, so it has no symbol and no body. that is what separates it from `intrinsic`,
    // which names an LLVM intrinsic and still produces a real llvm::Function.
    //
    // the surface still lives in the stdlib (`function size_of<T>() : usize;` in `namespace mem`)
    // so that the name, the namespace, the documentation and the "unknown function" diagnostic all
    // come from Echo source rather than being hardcoded in the parser
    enum class BuiltinKind
    {
        t_size_of,
        t_align_of,

        // the two ways a program stops itself. unlike the two above they take arguments, return
        // void and are not generic - so they are the first builtins to rely on `is_builtin()`
        // rather than the `is_generic()` guard that happens to sit ahead of it in TypeLowering
        //
        // they are builtins rather than library functions for one reason: the message carries the
        // *call site's* source location, which nothing a library can be handed knows
        t_die,
        t_assert,

        // how many strong references a class handle has. the **first builtin that is both generic and
        // takes an argument** - the two above take arguments and are concrete, the two at the top are
        // generic and argument-less - so it fits neither family's shape and owns its own arm.
        //
        // a builtin rather than a library function because the count is a word inside the heap block
        // (ClassBox::strong_index) with no Echo spelling reaching it. its parameter is a **borrow**, not
        // a value: a by-value class parameter is +1, so the answer would be one too high at every call,
        // which is exactly the question the caller is asking
        t_ref_count,

        // and the other count in the same block (ClassBox::weak_index) - how many handles need it to stay
        // readable. the same shape as t_ref_count in every respect, so the two share their argument check
        // and their codegen arm and differ only in which word they read
        //
        // it exists for a narrower reason than its sibling: a `weak<T>` is only correct if the two counts
        // move independently, and *nothing observable from Echo distinguishes a balanced pair from a leaked
        // one*. so the corpus pins both counts directly rather than inferring them from destructor output,
        // which is what makes the reference cycle in tests_eco/classes an assertion instead of a hope
        t_weak_count,

        // print a value with its type and, for anything with properties, its whole structure. the same
        // shape as the two counts above - generic, one borrow argument - but a different *kind* of
        // builtin from all five: they fold to a constant or read one word, and this one **emits**. it is
        // also the first whose lowering creates basic blocks, which is why its renderer is a codegen
        // subsystem of its own rather than an arm on ExprCodegen
        //
        // a builtin rather than a library function because everything it prints is a compiler fact with
        // no Echo spelling: the name of a type, the names and order of its properties, and the layout it
        // reads them out of. `echo` covers one scalar and refuses a struct outright, which leaves
        // debugging a value as one `echo` per field with the types remembered by hand
        //
        // its parameter is a **borrow** for a sharper version of ref_count's reason: a by-value class
        // argument is +1, so a printer would report a count it created itself, and a by-value struct
        // argument is a copy - so a struct that declares a copy constructor would run it, and the printer
        // would be printing something other than the value it was handed
        t_dprint,
    };

    // resolves a builtin name to its kind, or nullopt when the name is not one. the single place
    // that knows the set, so the parser can reject an unknown name where the attribute is written
    // rather than letting it fail deep inside codegen
    bool is_known_builtin(const std::string &name);
    BuiltinKind builtin_kind_for(const std::string &name);

    // **which argument is the message, if any?** the position `die`/`assert` fold into the abort
    // text along with the source location, and therefore the one argument that has to be a string
    // literal rather than any expression of the right type
    //
    // one owner because two subsystems ask and they must agree: AST::TypeChecker validates the
    // shape at that index, and ExprCodegen reads the text from it. spelled 0 and 1 in both, they
    // could drift into checking one argument and folding another - and the failure is silent,
    // because a non-literal simply yields no detail rather than an error
    std::optional<size_t> builtin_message_index(BuiltinKind kind);
};

#endif
