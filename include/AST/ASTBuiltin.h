#ifndef ASTBUILTIN_H
#define ASTBUILTIN_H

#pragma once

#include "AST/ASTValueType.h"

#include "Token.h"

#include <cstdint>
#include <optional>
#include <string>

namespace AST
{
    class Collector;
    class FunctionDeclNode;

    // the compiler builtins a function declaration can be bound to with `#[echoc::builtin: ...]`
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

        // **is this an integer primitive?** the same shape as the two above, folded from
        // ValueType::is_integer_type. what lets `sort()` skip stability where stability cannot
        // be observed, and what a later radix sort would ask. an unbound T stays "not yet"
        t_is_integer,

        // **`T::min()` / `T::max()` on an integer primitive.** AST facts about the width, the
        // same table a literal's range check reads (IntegerSize). interned by
        // AST::integer_bound_decl rather than written in the stdlib: a primitive has no
        // ComplexType to hang a static on
        t_integer_min,
        t_integer_max,

        // **the runtime identity of a type**, as a value a program can hold. `type_id<T>()` is the
        // address of a `linkonce_odr` global: a class's existing typeinfo, an interface's itype, or
        // a one-byte `.typeid` minted for everything else. the same identity `instanceof` compares,
        // wrapped so it can be a map key. not a foldable integer - the answer is an address
        t_type_id,

        // **owning erasure of a class handle**, no interface required. `erased::from<T>` stores the
        // handle and T's release thunk; `assume<T>` promises the slot is a T. retain/release are
        // the copy and destructor of the `erased` struct, answered here because the thunk is a
        // compiler symbol
        t_erased_from,
        t_erased_retain,
        t_erased_release,
        t_assume,

        // load through a borrow and leave the slot dead. a call, so OwnershipPass inserts no copy.
        // `mv` is for a variable; this is for a slot a container manages itself
        t_take,

        // store into a place that holds nothing. `=` would drop the old value, and a slot out of
        // `mem::alloc` has none. the value arrives by value, so this hands over an owner
        t_init,

        // the two ways a program stops itself. unlike the two above they take arguments, return
        // void and are not generic - so they are the first builtins to rely on `is_builtin()`
        // rather than the `is_generic()` guard that happens to sit ahead of it in TypeLowering
        //
        // they are builtins rather than library functions for one reason: the message carries the
        // *call site's* source location, which nothing a library can be handed knows
        t_die,
        t_assert,

        // the unspellable stop a `guard` without `else` lowers to. no arguments: the headline
        // and the location come from the call site, which is the `guard` keyword. distinct from
        // `die` so `--no-stdlib` can mint one without a `string` type and without the stdlib
        // declaration
        t_unwrap_abort,

        // the crash-hook trio. they exist so AbortCodegen is the only thing that touches
        // `__eco_crash_hook` and the default print: set/take are the store, default_hook is the
        // print with no exit. not library functions, because the store is a compiler global
        t_crash_set_hook,
        t_crash_take_hook,
        t_crash_default_hook,

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

        // `die` for a stdlib check that has to stay inlinable. one cold, argument-free stop per message;
        // the happy path is a compare and a branch. message is a literal, no location
        t_cold_die,

        // the per-process seed `hash::of` folds in. a plain load; constant 0 under `--fixed-hash-seed`.
        // that pins iteration order, it does not restore hashes from before mix
        t_hash_seed,

        // the seven `mem::atomic::` verbs. a builtin over a borrow, same argument as `take`/`init`:
        // the compiler is the only thing that can emit an `atomicrmw`, and an ordering is a claim
        // about two accesses nothing in the language can check - so there is no ordering parameter
        // and the surface is sequentially consistent
        //
        // they do **not** own raw storage. `builtin_owns_raw_storage` answering true would exempt
        // the `T&` from `check_unsafe_promotion` and launder a raw address into a trusted borrow.
        // AST::atomic_operand_refusal is the type gate; TypeChecker asks it once
        t_atomic_load,
        t_atomic_store,
        t_atomic_add,
        t_atomic_sub,
        t_atomic_exchange,
        t_atomic_compare_exchange,
        t_atomic_fence,

        // the `simd::` verbs. a builtin rather than an LLVM intrinsic because splat is a
        // shufflevector, load/store choose the element's alignment, select is CreateSelect,
        // bitmask is a portable iN bitcast for N<=8 and two 8-lane addv of byte powers for
        // N=16, and the array conversions are insert/extract loops. `any`/`all` stay
        // `#[echoc::intrinsic:]` because LLVM already names those reductions
        t_simd_splat,
        t_simd_load,
        t_simd_store,
        t_simd_select,
        t_simd_bitmask,
        t_simd_from_array,
        t_simd_to_array,

        // the plugin ABI table. version/fingerprint/slot_count are AST facts
        // folded at the call; symbol and slot_symbol emit string constants;
        // bind_unchecked seats the struct from a slot pointer and an erased
        // image. the surface lives in `abi::`
        t_abi_version,
        t_abi_fingerprint,
        t_abi_slot_count,
        t_abi_symbol,
        t_abi_slot_symbol,
        t_abi_bind_unchecked,

        // `std::embed::text` / `std::embed::bytes`. the path is a string literal, the bytes
        // land in the binary as a `string` or a `string::view`. AST::embed_plan_for is the owner
        t_embed_text,
        t_embed_bytes,
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
    // text along with the source location when that argument is a literal.
    //
    // the index is the one ExprCodegen reads, so a literal `die` and a runtime `die` cannot
    // fold different arguments. whether that argument *must* be a literal is
    // builtin_message_must_be_literal - a different question, asked by TypeChecker
    std::optional<size_t> builtin_message_index(BuiltinKind kind);

    // **must the message be a compile-time literal?** `assert` and `cold_die` yes (the stripped check /
    // the thunk is keyed on the text). `die` no: the text is written at the crash
    bool builtin_message_must_be_literal(BuiltinKind kind);

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

    // the bits `T::min()` / `T::max()` fold to. signed values are sign-extended to 64 bits.
    // IntegerSize is the one extrema table: a literal's range check already reads it
    uint64_t integer_bound_bits(ValueTypePrimitive primitive, bool is_max);

    // **`T::min()` / `T::max()` on an integer primitive.** interned, `#[echoc::builtin:]`, no symbol.
    // null when `name` is neither, or `type` is not an integer. `at` names the decl on first mint.
    // a primitive has no ComplexType to hang a static on, so CallResolver asks this rather than
    // find_static_functions
    FunctionDeclNode *integer_bound_decl(
        Collector &collector,
        const ValueType &type,
        const std::string &name,
        const TokenReference &at
    );
};

#endif
