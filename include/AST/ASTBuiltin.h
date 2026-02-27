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

        // **the two ownership questions, asked from Echo.** the same shape as the two above - generic,
        // argument-less, folded to a constant at the call site - but what they fold is an *AST* fact
        // rather than a layout one: AST::classify_copy and AST::needs_destruction, verbatim
        //
        // they exist so a library body can be written against the taxonomy instead of guessing at it.
        // `array<T>`'s copy constructor was a bitwise `mem::copy` and its destructor freed the buffer
        // without touching the elements, both because "there is no way to ask whether `T` owns
        // anything" - so an `array<string>` compiled, double-freed, and had no diagnostic anywhere
        //
        // builtins rather than a `contract::` interface a `T` could conform to, which is the other way
        // the question is expressible: conformance is something a type's author opts into, and the
        // answer here must hold for every type including the ones written before the question existed.
        // the compiler already knows, and one owner that already decides it is the point
        t_is_trivially_copyable,
        t_needs_destruction,

        // **move the value out of a place**, leaving the storage behind it dead
        //
        // The verb the two predicates above are useless without. Knowing `T` owns something buys
        // nothing if the only way to get an element out of a raw buffer is to *copy* it, which leaves
        // the slot holding a second owner nothing will ever release.
        //
        // What a container needs is the read that ends the source's claim, and Echo has no spelling for
        // one: `mv` transfers a *variable*, and an element of a buffer this type is itself managing is
        // not one.
        //
        // Its lowering is one load through the borrow, and everything else follows from it being a
        // **call**. A call result is not a place, so AST::OwnershipPass inserts no copy, and the owner
        // it hands back is destroyed by the ordinary frame drop of wherever it lands. So `pop()` is a
        // move, and a destructor's element loop is a take whose value is dropped at the end of the
        // iteration - with no arm for either anywhere in that pass.
        //
        // Unsafe by construction, and squarely in `mem::` for that reason: taking twice from one place
        // duplicates ownership exactly as freeing twice duplicates a free. AST::TypeChecker refuses the
        // two misuses that have a correct spelling instead - a source that is not a place, and a whole
        // local variable, which is what `mv` is for
        t_take,

        // **move a value into a place that holds nothing**, which is the mirror of `take` above and the
        // other half of what a container needs.
        //
        // `take` empties storage the compiler is not accounting for; this fills it. Writing to that
        // storage with an ordinary `=` is a *re*-assignment, so AST::OwnershipPass ends whatever the
        // destination held - and for a slot straight out of `mem::alloc` that is a destructor call over
        // whatever bytes the allocator handed back. There is no other spelling: `array<T>` gets the rule
        // from the *shape* of its append operator (AST::IndexExprNode::is_append, a syntactic question the
        // parser answers), and a container whose slots are not appended to has no shape to get it from.
        //
        // deliberately **not** an ordinary library function. `function init<T>(T& $p, T $v) { $p = $v; }`
        // is exactly the re-assignment it exists to avoid, one level further in - so the seam has to be a
        // builtin, where the write is emitted rather than written
        //
        // Its lowering is one store through the borrow, and what makes that correct is that the value
        // *arrived by value*. The caller's copy already happened, so this hands over an owner rather
        // than duplicating one, and nothing is owed a release.
        //
        // It shares `take`'s place rule exactly - AST::is_unaccounted_storage, one predicate with two
        // readers - and it is unsafe in the same direction and for the same reason. Initializing a place
        // that already holds a value leaks that value, the way taking twice from one place duplicates
        // ownership. Squarely in `mem::` for that reason
        t_init,

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

        // print a value with its type and, for anything with properties, its whole structure.
        //
        // The same shape as the two counts above - generic, one borrow argument - but a different *kind*
        // of builtin from all five. They fold to a constant or read one word; this one **emits**. It is
        // also the first whose lowering creates basic blocks, which is why its renderer is a codegen
        // subsystem of its own rather than an arm on ExprCodegen.
        //
        // A builtin rather than a library function, because everything it prints is a compiler fact with
        // no Echo spelling: the name of a type, the names and order of its properties, and the layout it
        // reads them out of. `echo` covers one scalar and refuses a struct outright, which leaves
        // debugging a value as one `echo` per field with the types remembered by hand.
        //
        // Its parameter is a **borrow**, for a sharper version of ref_count's reason. A by-value class
        // argument is +1, so a printer would report a count it created itself. A by-value struct
        // argument is a copy, so a struct that declares a copy constructor would run it - and the printer
        // would be printing something other than the value it was handed
        t_dprint,

        // the C allocator, reached through the compiler's own allocation seam rather than bound as an
        // `extern` symbol. what `mem::alloc`, `mem::realloc` and `mem::free` are written on top of, and
        // between them the whole of where an `array<T>`'s storage, a `str::buf`'s bytes and every raw
        // buffer in the language come from
        //
        // They were `extern malloc`/`realloc`/`free` until the compiler needed to be able to say how
        // much memory a program had outstanding. Two sites allocated - these, and the class runtime's
        // box - and because nothing saw both, the question had no answer at all rather than an expensive
        // one.
        //
        // Builtins rather than externs, so the seam has *one* owner (Compiler::LLVM::MemoryCodegen)
        // instead of two spellings of one symbol name that nothing checks against each other.
        //
        // The **first concrete, value-returning builtins**. `die`/`assert` are concrete and push
        // nothing; `size_of`/`ref_count` return a value and are generic. So they are also the first that
        // need no arm in AST::TypeChecker - an ordinary declared signature is one AST::CallResolver
        // already checks, which `T&` and a `void`-accepting parameter are not
        t_alloc_bytes,
        t_realloc_bytes,
        t_free_bytes,

        // how many allocations the three above have outstanding. reads the counter the seam maintains,
        // which is what makes a leak something the corpus can *assert* rather than infer from destructor
        // output - the same argument t_weak_count is here for, one level further down
        //
        // it is the only builtin whose availability is conditional: without --track-allocations there is
        // no counter, and answering 0 would be a lie in the one shape a person cannot tell from the truth
        // they hoped for. AST::TypeChecker refuses it there, at the call site, with a location
        t_live_allocations,

        // the three words the platform hands `main`, read back out of the globals the entry point's
        // prologue stored them in (Compiler::LLVM::ProcessCodegen)
        //
        // builtins because there is no other way down: the values exist only as `main`'s arguments,
        // and Echo has no globals to park them in - a file-scope variable is a stack slot in whichever
        // function is being emitted, and in a library module it is dropped without a word. An `extern`
        // cannot reach them either, since `environ` is a *data* symbol and `argv` is not a symbol at all
        //
        // they are the raw pointers rather than anything shaped, deliberately: `std::env` builds the
        // iterators, the bounds checks and the `KEY=VALUE` split on top in Echo, where they can be read.
        // The compiler's whole contribution is three loads
        t_process_argc,
        t_process_argv,
        t_process_envp,

        // stop with a chosen exit code, as opposed to `die`'s hardcoded 1
        //
        // a builtin rather than `extern exit` for two reasons. Compiler::LLVM::AbortCodegen already
        // declares that symbol, with NoReturn, so a second Echo-side spelling would be two declarations
        // of one symbol that nothing checks against each other - the mistake the raw-memory trio above
        // exists to have stopped making. And a `void`-returning extern is a call that *returns* as far
        // as the rest of the compiler knows, so `function f() : int32 { env::exit(1); }` would be a
        // missing return; AST::scope_exit_kind gives it the arm `t_die` has instead
        t_exit,
    };

    // **can this builtin be answered before codegen, and if not, why not?**
    //
    // three answers rather than a bool, because the two ways "no" happens are two different sentences
    // to whoever wrote the call. a layout query *could* be foldable one day and is not today; a
    // `dprint` never will be, and telling someone to wait for the first is useless advice about the
    // second
    enum class BuiltinFoldability
    {
        // AST::classify_copy and AST::needs_destruction, verbatim - the tree and nothing else, so
        // AST::const_fold can answer them from a ValueType alone
        t_ast_fact,

        // size_of / align_of. an llvm::DataLayout and a TypeRegistry, which exist only at codegen -
        // and asking the AST for a struct's stride is asking it for an ABI it does not model. what is
        // missing is a target layout at AST level, not an arm here
        t_needs_layout,

        // not a query at all: it allocates, prints, stops the program, or moves a value out of a place
        t_not_a_query,
    };

    // here rather than as a switch inside AST::const_fold, and exhaustive for builtin_message_index's
    // reason: a builtin added without an arm is a compile error rather than one that silently answers
    // "not foldable", which would cost a `const if` a refusal nobody could explain
    //
    // two readers, which is what earns it a home of its own: AST::const_fold, and
    // ExprCodegen::gen_builtin_call's routing of the four type queries
    BuiltinFoldability builtin_foldability(BuiltinKind kind);

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

    // **does a call to this builtin never come back?** `die` and `exit` do not, so a statement that is
    // one leaves the function just as surely as a `return` does.
    //
    // here rather than as a switch inside AST::scope_exit_kind, and exhaustive for builtin_message_index's
    // reason: a builtin added without an arm is a compile error rather than one that silently answers
    // "control continues", which would cost a spurious missing-return diagnostic or a block codegen leaves
    // unterminated
    bool builtin_never_returns(BuiltinKind kind);

    // **does this builtin deliberately name storage the compiler is not accounting for?** `mem::take`
    // empties such a slot and `mem::init` fills one, and they are the only two - every other builtin
    // takes an ordinary value or an ordinary borrow.
    //
    // one owner because the two readers are opposite halves of one boundary and a disagreement is
    // silent in the dangerous direction: AST::TypeChecker::check_raw_storage_argument refuses these two
    // over *accounted* storage, and check_unsafe_promotion exempts the borrow they are handed from the
    // `unsafe` rule. spelled as `is_builtin()` the exemption covered every builtin, so `dprint`,
    // `mem::ref_count` and `mem::weak_count` - which take a plain `T&` - laundered a raw address into a
    // trusted borrow with nothing asked of the author
    bool builtin_owns_raw_storage(BuiltinKind kind);
};

#endif
