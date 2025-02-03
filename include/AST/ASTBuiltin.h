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
