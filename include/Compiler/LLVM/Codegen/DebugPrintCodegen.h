#ifndef DEBUGPRINTCODEGEN_H
#define DEBUGPRINTCODEGEN_H

#pragma once

#include "AST/ASTValueType.h"
#include "Compiler/LLVM/Codegen/LValueCodegen.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace llvm
{
    class BasicBlock;
    class StructType;
    class Value;
};

namespace AST
{
    class ComplexType;
};

namespace Compiler::LLVM
{
    struct CodegenContext;

    // the `dprint` builtin: one value, rendered with its type and - for anything with properties - its
    // whole structure
    //
    // a subsystem rather than an arm on ExprCodegen for two reasons that are the same reason. it carries
    // state across a recursion - the buffer and the cycle path below - where every other expression arm
    // is re-entrant through accept(); and it **creates basic blocks**, which is what a class handle and a
    // wrapped optional cost. neither belongs on a class whose whole contract is "leave one value on the
    // stack". it is the same split gen_ref_count_builtin already makes against ClassCodegen::gen_count
    //
    // **the whole shape is static.** which properties exist, their names, their types and the
    // indentation are all known while lowering, so the only things that are not compile-time constants
    // are the leaf values themselves. that is what makes the buffer possible: a struct of ten scalars is
    // one printf with ten varargs, not twenty-one calls
    class DebugPrintCodegen
    {
    public:
        DebugPrintCodegen(CodegenContext &ctx) : _ctx(ctx) {};

        // renders `value` - the *storage* of the argument, not a loaded value - and everything reachable
        // from it, terminated by a newline. pushes nothing onto the value stack: `dprint` returns void,
        // exactly as `die` does
        //
        // takes an LValue rather than an llvm::Value because a struct is walked by GEP, and because the
        // storage type is the one thing an opaque pointer cannot tell us. producing it is the caller's -
        // see ExprCodegen::gen_dprint_builtin, where the address is simply the argument the resolver
        // already wrapped in an AddrOfExprNode
        void gen_dprint(const LValue &value);

    private:
        CodegenContext &_ctx;

        // how deep the walk goes before it gives up and prints `<max depth>`. a guard, not a feature: the
        // path set below cannot see a chain of *distinct* types that never repeats - `A` holding a `B`
        // holding a `C` - and without a counter that recursion is bounded only by the compiler's stack
        static constexpr size_t max_depth = 8;

        // flush once the vararg list reaches this, even with nowhere to branch. a 200-property struct
        // otherwise emits one printf whose IR line no one can read, and printf's own vararg handling is
        // not free at that width either. costs nothing in the common case
        static constexpr size_t max_pending_args = 32;

        // -- the buffer --------------------------------------------------------------------------
        //
        // pending static text, and the varargs it refers to. text accumulates until something forces a
        // flush, which is only ever a *branch*: a class handle's null test and a wrapped optional's
        // `__has` test are the two runtime questions this printer asks, and one printf cannot straddle a
        // branch
        //
        // `_pending_block` is the block the buffer was opened in, asserted against the builder's current
        // block on every append - a buffer carried across a block boundary emits its text into the wrong
        // arm, and there is nothing in the resulting IR that would look wrong
        std::string _pending_format;
        std::vector<llvm::Value *> _pending_args;
        llvm::BasicBlock *_pending_block = nullptr;

        // the ComplexTypes on the path from the root to whatever is being rendered right now. a type that
        // reaches itself - `class Node { Node? $next; }` - is cut with `<cycle>` on the second occurrence
        // rather than followed, because **the printer walks the static type**, and the static type of a
        // list is infinitely deep however short the list is
        //
        // keyed on the pointer, which is exactly right: ValueType equality is pointer identity on
        // ComplexType, so `Box<int32>` and `Box<float32>` stay two types and only genuine self-reference
        // is cut. a vector rather than a set - it is never longer than max_depth, and push/pop around one
        // recursion level is the whole of its use
        std::vector<const AST::ComplexType *> _path;

        // -- the walk ----------------------------------------------------------------------------

        // one value. `label` is "" at the root and "$name = " for a property, which is the only
        // difference between the two positions - so there is one renderer rather than two that drift.
        // emits no leading indentation and no trailing newline; render_property owns both
        //
        // `display_type` overrides the name printed in the brackets without changing the type that is
        // *read*. exactly one caller needs the two to differ: a present `T?` is read as a `T` - the tag
        // is stripped and the payload GEP'd out - but it must still print as `[T?]`, or nothing in the
        // output distinguishes an optional holding 12 from a plain int32 holding 12
        void render(
            const LValue &place, std::string_view label, size_t depth,
            const AST::ValueType *display_type = nullptr);

        // `[T] <label>{ ... }` over a property layout, or `[T] <label>{}` when there are none.
        //
        // `address` points at the properties themselves and `layout` is the struct they sit in - for a
        // class that is the *payload*, already GEP'd past the header, which is exactly what lets a class
        // and a struct share this. the caller resolves both because only it knows which of the two it
        // has: a class value's own llvm type is an opaque handle, not its layout
        // takes the *name* rather than the type for render's display_type reason: a present `Point?`
        // reads as a `Point` and must still print as `[Point?]`
        void render_properties(
            llvm::Value *address, llvm::StructType *layout, const AST::ComplexType &complex,
            std::string_view type_name, std::string_view label, size_t depth);

        // one line of a struct body: the indent, the recursive render, the newline. property order is
        // declaration order, which is also llvm element order - see
        // TypeLowering::create_llvm_struct_for_instance - so the property's own index is the GEP index,
        // with no second table to keep in step
        void render_property(
            llvm::Value *address, llvm::StructType *layout, const AST::ComplexType &complex,
            size_t index, size_t depth);

        // the llvm struct a value's properties sit in: its own type for a struct, its payload for a
        // class. one place, because getting it wrong for a class GEPs into the reference count
        llvm::StructType *property_layout_of(const AST::ValueType &type, const AST::ComplexType &complex);

        // **why this recursion stops**, as one answer rather than one per descending arm: `<cycle>` when
        // the type is already on the path being rendered, `<max depth>` past the depth limit, and null to
        // carry on. a class arm and a struct arm that cut on different rules would make the output depend
        // on which of the two a value happened to be reached through
        const char *cut_reason(const AST::ComplexType &complex, size_t depth) const;

        // a class handle: `[Foo] null` or the expanded payload, decided at runtime. one of the two arms
        // that flush and branch, and the reason gen_dprint cannot be a straight-line emitter.
        // **does not retain** - the printer reads, it does not own, or `ref_count($x)` would read
        // differently on either side of a `dprint($x)`
        // `type_name` is passed in rather than rebuilt: render() already derived it, and
        // get_type_desciption walks the type and allocates
        void render_class(
            const LValue &place, const AST::ValueType &type, std::string_view type_name,
            std::string_view label, size_t depth);

        // a `T?` that had to be tagged - `{ i1 __has, T }`, see OptionalBox - which is every optional
        // except the ones with a null representation of their own. the same branch shape as a class
        // handle over a different condition, which is why that shape is a helper
        void render_optional(
            const LValue &place, const AST::ValueType &type, std::string_view type_name,
            std::string_view label, size_t depth);

        // `"..."` from a bound core `string` or `string::view`, inlined into the surrounding printf with
        // `%.*s` rather than written out separately. only reached when a binding exists - `--no-stdlib`
        // leaves the core types unbound, and a string then renders as the ordinary struct it is
        void render_string(const LValue &place, const AST::ValueType &type);

        // a leaf: the loaded value and the conversion its primitive takes, including the two that are not
        // a plain table lookup - a bool selects between two literal pointers, and the two float rows are
        // overridden so that 42.69 prints as `42.69` rather than `42.690000`
        void render_primitive(llvm::Value *value, const AST::ValueType &type);

        // -- the buffer's operations ---------------------------------------------------------------

        // appends literal text, escaping `%` so a name containing one cannot become a conversion. no
        // identifier can today; the escape is here so the invariant is a property of this function rather
        // than of the identifier grammar
        void text(std::string_view literal);

        // appends one conversion and the value it consumes. the value must **already** be promoted: C's
        // default argument promotions are mandatory before a variadic call, and the promotion is part of
        // the conversion rather than a step after it - see PrintfConversion.h
        void arg(std::string_view conversion, llvm::Value *value);

        // appends one raw address, as `0x` and the hex of the pointer read as an integer.
        //
        // **deliberately not `%p`**, whose rendering C leaves to the implementation: glibc prints a null
        // one as `(nil)` where the BSDs print `0x0`, so a program's own output differed by the libc it
        // happened to be linked against. that is a fact about the *emitted program*, not about a test -
        // the goldens that caught it are only what made it visible
        void address(llvm::Value *pointer);

        // emits the pending printf and empties the buffer. a no-op when nothing is pending, so every
        // branch site can flush unconditionally
        void flush();

        // opens a two-armed branch, flushing first. hands back the join block, having left the builder at
        // the start of the *true* arm; `on_false_out` receives the other. close_arm branches to the join
        // from wherever the arm actually ended, which need not be where it started - an arm may itself
        // have branched
        llvm::BasicBlock *open_branch(
            llvm::Value *condition,
            const char *label,
            llvm::BasicBlock *&on_false_out
        );
        void close_arm(llvm::BasicBlock *join);

        // `"  " * depth`. the nesting level is a compile-time constant, so an indent is literal text like
        // every other part of the frame
        static std::string_view indent_for(size_t depth);
    };
};

#endif
