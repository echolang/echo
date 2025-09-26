#ifndef ASTARGUMENTFIT_H
#define ASTARGUMENTFIT_H

#pragma once

#include "AST/ASTConformance.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"

namespace AST
{
    // how well an argument answers a parameter. declaration order is best to worst, and that
    // order *is* the overload ranking - AST::match_function compares these values directly, so
    // reordering the enum changes which overload a call resolves to
    enum class ArgumentFit
    {
        // the declared types are identical
        t_exact,

        // convertible without losing anything the type system tracks: a `T&` handed to a
        // `ptr<T>`, a `int32&` handed to a `const int32&`
        t_widening,

        // the parameter is a borrow and the argument is a place, so its address is taken
        // implicitly. see AST::CallResolver::coerce_arguments, which performs exactly this case
        t_borrow,

        // the same borrow, into a parameter that promises only to read: a mutable place handed to a
        // `const T&`. **the borrow-level mirror of t_widening**, and ranked below t_borrow for that
        // rank's own reason - the argument gains a promise it did not have, so an overload that
        // wanted no promise fits better.
        //
        // without this rank the two are one, and `f(Foo&)` beside `f(const Foo&)` is ambiguous for
        // every mutable argument - which is exactly the pair a read-only `operator []` forms with the
        // writable one, so const overloading is unusable without it
        t_borrow_const,

        // the mirror of it: the argument is a place holding a non-nullable borrow and the parameter
        // wants the value, so the read goes one level through. PointerAdjuster::as_value_for already
        // emits that deref - this rank is only the *scoring* of it, which was missing
        //
        // beside t_borrow rather than anywhere else because it is the same class of adjustment, and
        // the two can never compete for one argument: one needs a pointer parameter and a value
        // argument, the other the reverse
        t_read_through,

        // a numeric conversion that stays inside its family and cannot lose anything: a wider
        // integer of the same signedness, or a wider float. ranked above a plain conversion so
        // that `w(int64)` and `w(float64)` called with an int32 resolves to the integer one
        // instead of being ambiguous
        t_promotion,

        // any other primitive that reaches the parameter through TypeLowering::coerce_value - a
        // narrowing, a sign change, int to float. always possible, never free
        t_conversion,

        // a class handle reaching a parameter of an interface it conforms to - the value keeps its
        // object and loses its static type, gaining a vtable. **below every built-in conversion
        // above**, so an overload taking the concrete class always beats one taking the interface: the
        // widening is what lets a caller hand a `Circle` to a `Drawable` parameter without writing
        // anything, never a way to hide a better-matching overload. that is `t_declared_conversion`'s
        // reasoning one rank earlier, and above it because this one is the compiler's own rule while
        // that one is something a type declared about itself
        //
        // deliberately **not** folded into is_implicitly_convertible, which would rank it t_widening
        // and make erasing a static type look as free as `T&` -> `ptr<T>`
        t_interface_widening,

        // the argument's own type declared how to convert itself, `#[implicit]`. below every
        // built-in conversion above rather than beside them, because that is what makes an
        // overload taking the owning type always beat one taking the window: this is the
        // fallback that lets a caller take the cheap view without writing anything, never a
        // way to hide a better-matching overload. see AST::find_implicit_conversion
        t_declared_conversion,

        // the parameter is a borrow and the argument is a value with no storage at all - a literal,
        // a call result, an arithmetic result - so a temporary is materialised to hold it and *its*
        // address is passed. AST::OwnershipPass binds the temporary and destroys it once the call has
        // returned (AST::TemporaryBindExprNode); AST::CallResolver writes the same AddrOfExprNode
        // t_borrow gets, because the difference between the two is a question about the operand's
        // shape and not one codegen should be able to see
        //
        // **the last real rank, below even t_declared_conversion**, and for a reason none of the ranks
        // above share: every one of them describes how a value that already exists is read or
        // converted. this one adds a *declaration* to the program - an alloca, a lifetime, possibly a
        // destructor call. so it is the fallback that lets a caller hand a value where a borrow is
        // wanted, never a way to hide a better-matching overload - which is t_interface_widening's and
        // t_declared_conversion's reasoning one and two ranks earlier
        //
        // concretely: `w(int64)` beats `w(int32&)` for `w(42)`, so a free integer promotion is not
        // out-ranked by fabricating storage; and t_borrow, six ranks up, means a borrow of real storage
        // always beats a borrow of a value the compiler had to invent
        //
        // below t_declared_conversion specifically because the two *compose* on one argument -
        // `f('hello')` against a view parameter converts first and then borrows the conversion's result
        // - and CallResolver::coerce_arguments re-asks the fit after that wrapping. keeping this last
        // makes the re-ask monotone: the coercion can only ever move the fit downward, never below what
        // the matcher already scored the candidate at
        //
        // the compiler cannot tell a mutating callee from a reading one, so a write through a `T&`
        // bound to a literal lands in storage nobody will read again. Echo already has the spelling
        // that says which - `const T&` - and gating this rank on it instead would refuse `f('hello')`
        // against the stdlib, whose borrow parameters are written bare
        t_borrow_temporary,

        // and the read-only form of that one, t_borrow_const's mirror at the bottom of the ranking.
        // both borrow arms owe the distinction or const overloading works for a place and is
        // ambiguous for a temporary - `$a[0]` resolving where `f()[0]` does not
        t_borrow_temporary_const,

        // the argument's type says nothing yet: a string literal, an unbound `null`, a member of
        // an incomplete struct, a mixed-operand binary expression, or anything still mentioning a
        // type parameter. neutral - it neither qualifies nor disqualifies a candidate
        t_undetermined,

        // no conversion exists
        t_none,
    };

    // a numeric conversion that keeps the value intact *and* stays in its own family: a wider
    // integer of the same signedness, or a wider float
    //
    // crossing families is deliberately not a promotion even when no value is lost - int32 into
    // float64 is exact, but it is a bigger step than int32 into int64, and every language that
    // ranks conversions at all ranks it lower. a sign change is not one either: the bit pattern
    // survives but the meaning does not
    //
    // spelled out here rather than reusing ValueType::will_fit_into, which answers a different
    // question - it accepts a bool into any numeric and rejects int32 into float64 as a side
    // effect of how it tests signedness, neither of which is what a ranking wants
    inline bool is_value_preserving_promotion(const ValueType &from, const ValueType &to)
    {
        const auto from_size = get_primitive_size(from.get_primitive_type());
        const auto to_size = get_primitive_size(to.get_primitive_type());

        if (from.is_integer_type() && to.is_integer_type()) {
            return from.is_signed_integer() == to.is_signed_integer() && from_size <= to_size;
        }

        if (from.is_floating_type() && to.is_floating_type()) {
            return from_size <= to_size;
        }

        return false;
    }

    // **which parameters auto-borrow** - the type-level half of the borrow rule, on its own because
    // it has a second reader that has no expression to ask about: AST::unify_type anticipates this
    // wrapping during inference, since the implicit address-of is inserted *after* a type argument is
    // bound (`bump<T>(T &$v)` called as `bump($a)` has to bind T=int32 from a bare value).
    //
    // a nullable `ptr<T>` deliberately does not auto-borrow: taking an address is a decision the
    // caller should be able to see in the source (book/concept/pointers_and_refs_v2.md, "Passing to
    // functions"). that exclusion is the whole reason this is one predicate rather than two spellings -
    // the inference and the coercion disagreeing about it would name an instance the coercion then
    // never produces
    inline bool parameter_auto_borrows(const ValueType &param)
    {
        return param.is_pointer() && !param.is_nullable();
    }

    // **may a borrow of `from` satisfy `to`?** the const half of the two borrow arms, which compare
    // the pointee with make_mutable on both sides and so would otherwise hand a mutable borrow of a
    // const place to a parameter that may write through it.
    //
    // the same asymmetry is_implicitly_convertible already applies to a pointee (`int32&` reaches
    // `const int32&`, never the reverse) - stated here rather than reused from there because that one
    // compares two pointers and this compares a *value* against the pointee of one. taking an address
    // must not launder a promise the address-of was inserted for.
    //
    // the arms it does not gate: t_widening delegates to is_implicitly_convertible, which already has
    // the rule, and t_read_through is a read - copying a const value out of one is exactly what a
    // const borrow is for
    inline bool borrow_preserves_const(const ValueType &from, const ValueType &to)
    {
        return to.pointee().is_const() || !from.is_const();
    }

    // **the whole type half of a borrow arm**, const rule included: the pointee is compared with
    // make_mutable on both sides, so the const promise has to be checked separately, and the two are
    // one question rather than two lines an arm can be written with only half of.
    //
    // one helper for the same reason borrow_rank is one - the arms are documented as verbatim mirrors,
    // and a condition spelled twice is one that drifts. this is what leaves each arm differing only in
    // its *expression* test, which is the one thing that genuinely does differ between them
    inline bool borrow_type_matches(const ValueType &from, const ValueType &to)
    {
        return borrow_preserves_const(from, to)
            && ValueType::make_mutable(from) == ValueType::make_mutable(to.pointee());
    }

    // which of a borrow arm's two ranks this argument earns: `matched` when the argument's const-ness
    // is already the parameter's, `promised` when the parameter adds const the argument did not have.
    //
    // one helper over both arms rather than the test spelled twice, because they are documented as
    // verbatim mirrors and this is exactly the kind of rule that drifts between two copies - one of
    // them ranking and the other not is how `$a[0]` and `f()[0]` come to resolve differently
    inline ArgumentFit borrow_rank(
        const ValueType &from, const ValueType &to, ArgumentFit matched, ArgumentFit promised)
    {
        return from.is_const() == to.pointee().is_const() ? matched : promised;
    }

    // **did this fit score a wrapping in an address?** all four borrow ranks answer yes, because the
    // const pair differs from the plain one only in how it *scores* - the node AST::CallResolver
    // produces is the same address either way.
    //
    // named here, where the ranks are defined, rather than enumerated at the call site: each borrow
    // arm gains a `_const` twin by construction, so a fifth rank added to the enum and not to a
    // hand-written `if` is an argument that silently stops being addressed
    inline bool fit_is_borrow(ArgumentFit fit)
    {
        switch (fit) {
            case ArgumentFit::t_borrow:
            case ArgumentFit::t_borrow_const:
            case ArgumentFit::t_borrow_temporary:
            case ArgumentFit::t_borrow_temporary_const:
                return true;

            default:
                return false;
        }
    }

    // **what a `#[implicit]` conversion has to return to answer this parameter.** a borrow parameter is
    // answered by a conversion to its *pointee*: the conversion produces a value, and the borrow of that
    // value is a separate and later rank, which is exactly why t_borrow_temporary sits below
    // t_declared_conversion. so the question is asked one level in
    //
    // `const` is dropped for the borrow arms' reason - `const string::view&` is answered by the same
    // declaration a bare one is, and is_implicitly_convertible accepts the resulting `string::view&`
    // without a cast
    //
    // its own rule with two readers rather than a peel spelled at each: AST::argument_fit decides *that*
    // a conversion applies and AST::CallResolver retrieves *which*, and a disagreement between the two
    // trips the `assert` in convert_if_wanted. AST::find_implicit_conversion itself is deliberately left
    // exact - its comparison is what keeps a chain of conversions unsearchable - so the peel belongs
    // here, where the thing being described is a parameter position
    inline ValueType implicit_conversion_target(const ValueType &param)
    {
        return parameter_auto_borrows(param) ? ValueType::make_mutable(param.pointee()) : param;
    }

    // **the** rule for "does this argument answer this parameter" - the only implementation, with
    // three readers that each take a different amount of it:
    //
    //  - AST::match_function ranks with the whole ordering, which is what picks an overload;
    //  - AST::CallResolver's argument coercion reads only `t_borrow`, so a candidate the matcher
    //    accepted on the borrow arm is a candidate the coercion then actually wraps;
    //  - AST::TypeChecker reads only `!= t_none`, so a call it reports is a call resolution could
    //    not have chosen.
    //
    // it used to be three separate case analyses. the third was a hand-written copy in the type
    // checker whose pointer arm was plain is_implicitly_convertible, and the "first" was consulted
    // for concrete candidates while unify_type's tail filtered generic ones by yet another rule - so
    // a generic overload could be admitted by one and scored by the other with nothing to notice
    //
    // `expr` may be null when only the argument's type is known; the borrow rule needs the
    // expression, because only an expression can be a place. a caller that passes null therefore
    // declines the borrow arm, which is what a *cast* wants - a cast is not an address-of
    inline ArgumentFit argument_fit(const ValueType &from, const ExprNode *expr, const ValueType &to)
    {
        // no information. checked first so an undetermined argument can never be read as a
        // mismatch - the type checker and the monomorphizer's inference both already treat
        // unknown and void this way
        if (is_undetermined_type(from) || is_undetermined_type(to)) {
            return ArgumentFit::t_undetermined;
        }

        if (from == to) {
            return ArgumentFit::t_exact;
        }

        // **a weak fits a weak of the same class and nothing else.** so the equality above is the only
        // arm that can admit one, and everything below is skipped rather than merely happening to answer
        // no - the interface-widening arm would ask `conforms_to` about a type with no ComplexType, and
        // the declared-conversion arm would ask for a `#[implicit]` method on one. neither is a question
        // a weak can answer, and asking gets an assert rather than a false
        if (from.is_weak() || to.is_weak()) {
            // except through a borrow: a `weak<Foo>& $w` parameter takes the address of a weak place,
            // which is the ordinary auto-borrow and says nothing about the count
            //
            // a *place* only, and deliberately not the t_borrow_temporary arm at the bottom: a weak
            // exists to outlive the handle it watches, so binding one to storage that dies with the
            // statement is a shape nobody can have meant
            // the pointee comparison is the borrow arms', asked through the same helper so this does
            // not become a third spelling of "which pointee does a borrow accept" - const rule
            // included, which costs this arm nothing it wanted: a `const weak<Foo>` place handed to a
            // `weak<Foo>&` is the laundering borrow_preserves_const exists to refuse, and refusing it
            // here is the arm agreeing with its two mirrors rather than an exception to them
            if (parameter_auto_borrows(to) && expr != nullptr && is_place_expression(*expr)
                && borrow_type_matches(from, to)) {
                return ArgumentFit::t_borrow;
            }

            return ArgumentFit::t_none;
        }

        // before the borrow rule, so an argument that already fits a borrow parameter is not
        // wrapped in a second address-of. that ordering is load-bearing in
        // AST::CallResolver::coerce_arguments, where taking the address of a ptr<int32> for a
        // ptr<int32> parameter would build a ptr<ptr<int32>>
        if (is_implicitly_convertible(from, to)) {
            return ArgumentFit::t_widening;
        }

        // a borrow parameter takes the address of any place - which parameters those are is
        // parameter_auto_borrows, shared with the inference that has to predict this
        if (parameter_auto_borrows(to) && expr != nullptr && is_place_expression(*expr)
            && borrow_type_matches(from, to)) {
            return borrow_rank(from, to, ArgumentFit::t_borrow, ArgumentFit::t_borrow_const);
        }

        // reading one level *through* a borrow to fill a value parameter, which is what
        // `take($b)` means for a `Point& $b` and a `Point` parameter. the deref is inserted
        // afterwards by PointerAdjuster::as_value_for, exactly as the borrow arm's address-of is
        // inserted by CallResolver - so both arms score a wrapping that a later pass performs
        //
        // AST::unify_type already decays a pointer argument to its pointee for a value parameter
        // (its `allow_decay` arm), so without this rank inference named an instance that scoring
        // then rejected. with one candidate the disagreement was invisible, because matching rule 2
        // wins without consulting types at all; with two it was a hard "no overload accepts these
        // arguments", so adding an unrelated overload broke a call that had always compiled
        //
        // **non-nullable only.** reading through a `ptr<T>` that may be null is an unchecked
        // dereference, and the one narrowing that does emit a check goes the other way
        // (book/concept/pointers_and_refs_v2.md, "Nullability"). a nullable argument stays t_none,
        // which is what it already was
        if (from.is_pointer() && !from.is_nullable() && !to.is_pointer()
            && expr != nullptr && is_place_expression(*expr)) {
            if (ValueType::make_mutable(value_type_of(from)) == ValueType::make_mutable(to)) {
                return ArgumentFit::t_read_through;
            }
        }

        // whatever is left between two primitives is a conversion TypeLowering::coerce_value
        // knows how to emit. a pointer, struct or class on either side has no such table
        if (from.is_primitive() && to.is_primitive()) {
            return is_value_preserving_promotion(from, to)
                ? ArgumentFit::t_promotion
                : ArgumentFit::t_conversion;
        }

        // a class handle widening to an interface it conforms to. asked of AST::conforms_to, the one
        // answer to that question, so the ranking and the lowering cannot disagree about which
        // conversions exist
        //
        // **a class only.** a struct's conformance is a compile-time contract - it has no runtime type
        // for a dispatch to read - so a struct argument answers t_none here and the type checker
        // reports it against the destination, which is where the reason belongs
        if (to.is_interface() && from.is_class() && conforms_to(from, to)) {
            return ArgumentFit::t_interface_widening;
        }

        // a value converts to another type when its own type declared how - a method marked
        // `#[implicit]`, found by AST::find_implicit_conversion. last, and its own rank, so that
        // CallResolver can tell this case apart from the primitive conversions above by the rank
        // alone rather than by asking the lookup a second time.
        //
        // **asked of implicit_conversion_target rather than of `to`**, so a borrow parameter is answered
        // by a conversion to what it borrows. without that peel this arm could never fire for one at all,
        // the return type `string::view` never equalling the parameter type `string::view&` - so the
        // compose t_borrow_temporary's comment describes was unreachable rather than merely ungated
        //
        // **any operand, place or not.** the conversion is a call whose receiver is `$this`, so it needs
        // an address - and a non-place argument gets one the same way every other borrow argument does,
        // from the temporary AST::OwnershipPass mints for the `&` CallResolver writes around it. so
        // `f('hello')` composes: the literal is bound, the conversion reads it, and the borrow of *that*
        // takes the rank below (todo/A13c)
        if (expr != nullptr
            && find_implicit_conversion(from, implicit_conversion_target(to)) != nullptr) {
            return ArgumentFit::t_declared_conversion;
        }

        // and the same borrow parameter filled by a value that has no storage at all, which the
        // compiler answers by materialising a slot for it. the type test is the borrow arm's, through
        // borrow_type_matches rather than spelled again, so the two cannot come to different answers
        // about which pointee a borrow accepts - only the expression test differs, is_place_expression
        // there and can_bind_temporary here, and AST::storage_of makes those two mutually exclusive
        //
        // **last**, so that no arm above changes behaviour: a place reaches t_borrow twelve lines up and
        // returns there, never seeing this one. that is the whole of why this is an additive change
        //
        // **`from` must not itself be a pointer.** a `ptr<ptr<T>>` parameter would otherwise take a
        // pointer-typed non-place and ask AST::OwnershipPass for a slot it refuses to hand out ("a
        // pointer read out of a temporary is an address into it"), and the AddrOf CallResolver already
        // wrote would then reach gen_lvalue with nothing to address
        if (parameter_auto_borrows(to) && expr != nullptr && !from.is_pointer()
            && can_bind_temporary(*expr) && borrow_type_matches(from, to)) {
            return borrow_rank(
                from, to, ArgumentFit::t_borrow_temporary, ArgumentFit::t_borrow_temporary_const);
        }

        return ArgumentFit::t_none;
    }
};

#endif
