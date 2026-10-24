#ifndef ASTARGUMENTFIT_H
#define ASTARGUMENTFIT_H

#pragma once

#include "AST/ASTCFunction.h"
#include "AST/ASTConformance.h"
#include "AST/ASTConstness.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTNullability.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"

#include <cassert>

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

        // the mirror of it: the argument is a place holding a pointer (or a call that returned a
        // non-nullable borrow) and the parameter wants the value, so the read goes one level through.
        // PointerAdjuster::as_value_for already emits that deref - this rank is only the *scoring* of it
        //
        // a `ptr<T>` *place* ranks here too: the slot is certainly there, and that is the same auto-deref
        // a lone `f(T)` candidate already got via match rule 2. ranking it is what keeps `f(T)` beating
        // `f(T&)` once t_borrow_through exists - a by-value parameter still copies. a call that *returned*
        // a `ptr<T>` stays out: read_reaches_storage refuses it, and reading through an address that may
        // be absent is something the program has to say
        //
        // beside t_borrow rather than anywhere else because it is the same class of adjustment, and
        // the two can never compete for one argument: one needs a pointer parameter and a value
        // argument, the other the reverse
        t_read_through,

        // a `ptr<T>` place handed to a `T&` parameter: the address of the pointee, not a load of `T`.
        // CallResolver (direct) and PointerAdjuster (indirect) plant `AddrOf(Deref($p))`, the same
        // shape `$p->method()` plants in the parser, and TypeChecker::check_unsafe_promotion on that
        // AddrOf is the word - a raw address becoming a trusted borrow.
        //
        // **below t_read_through**, so `f(T)` still beats `f(T&)` for a `ptr<T>` argument: a by-value
        // parameter still copies, as a read should. not t_widening: `ptr<T>` is nullable and `T&` is
        // not, and is_implicitly_convertible staying false is the safety story
        t_borrow_through,

        // a numeric conversion that stays inside its family and cannot lose anything: a wider
        // integer of the same signedness, or a wider float. ranked above a plain conversion so
        // that `w(int64)` and `w(float64)` called with an int32 resolves to the integer one
        // instead of being ambiguous
        t_promotion,

        // borrow of a value with no storage: a temporary is minted and its address is passed.
        // below every rank that only reads, above every rank that changes the type. the enum's
        // order is not the arm order inside argument_fit
        t_borrow_temporary,

        // and the read-only form of that one, t_borrow_const's mirror. both borrow arms owe the
        // distinction or const overloading works for a place and is ambiguous for a temporary -
        // `$a[0]` resolving where `f()[0]` does not
        t_borrow_temporary_const,

        // any other primitive that reaches the parameter through TypeLowering::coerce_value - a
        // narrowing, a sign change, int to float. always possible, never free
        t_conversion,

        // a class handle reaching a parameter of an interface it conforms to - the value keeps its
        // object and loses its static type, gaining a vtable. **below every rank above**, so an
        // overload taking the concrete class always beats one taking the interface: the widening is
        // what lets a caller hand a `Circle` to a `Drawable` parameter without writing anything, never
        // a way to hide a better-matching overload. that is `t_declared_conversion`'s reasoning one
        // rank earlier, and above it because this one is the compiler's own rule while that one is
        // something a type declared about itself
        //
        // deliberately **not** folded into is_implicitly_convertible, which would rank it t_widening
        // and make erasing a static type look as free as `T&` -> `ptr<T>`
        t_interface_widening,

        // the argument's own type declared how to convert itself, `#[implicit]`. **below every
        // reading, widening and converting form above it**, because that is what makes an overload
        // taking the owning type always beat one taking the window: this is the fallback that lets
        // a caller take the cheap view without writing anything, never a way to hide a
        // better-matching overload. see AST::find_implicit_conversion
        //
        // the two still **compose** on one argument - `f('hello')` against a view parameter converts
        // first and then borrows the conversion's result - and CallResolver::coerce_arguments re-asks
        // the fit after that wrapping. the re-ask feeds AST::fit_is_borrow and nothing else, so which
        // way it moves the rank decides nothing; the matcher has already chosen the candidate by then
        t_declared_conversion,

        // the argument's type says nothing yet: a string literal, a `null` at a `T?` or a bare `T`,
        // a member of an incomplete struct, a mixed-operand binary, or a still-open type parameter.
        // a determined destination that refuses absence is t_none, above. neutral, not a mismatch
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
    // caller should be able to see in the source. that exclusion is the whole
    // reason this is one predicate rather than two spellings -
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
        const ValueType &from,
        const ValueType &to,
        ArgumentFit matched,
        ArgumentFit promised
    )
    {
        return from.is_const() == to.pointee().is_const() ? matched : promised;
    }

    // **did this fit score a wrapping in a bare address?** the four ranks that plant AddrOf of the
    // argument as it is. the const pair differs from the plain one only in how it *scores* - the node
    // AST::CallResolver produces is the same address either way.
    //
    // named here, where the ranks are defined, rather than enumerated at the call site: each borrow
    // arm gains a `_const` twin by construction, so a rank added to the enum and not to this switch is
    // an argument that silently stops being addressed. t_borrow_through is deliberately *not* one of
    // these: its wrap is AddrOf(Deref), asked of fit_is_borrow_through, or a bare AddrOf would build
    // `ptr<ptr<T>>`
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

    // **did this fit score a peel-then-borrow?** `ptr<T>` at a `T&` parameter. not fit_is_borrow:
    // that helper plants a bare AddrOf and would build `ptr<ptr<T>>`. the wrap is
    // AST::borrow_through_pointer
    inline bool fit_is_borrow_through(ArgumentFit fit)
    {
        return fit == ArgumentFit::t_borrow_through;
    }

    // **may a `ptr<T>` answer this `T&`?** the type half of t_borrow_through, on its own because
    // PointerAdjuster's indirect-call arm has to ask the same question after CallResolver has
    // already run on direct calls - two wrappers, one predicate, or a closure and an ordinary
    // function disagree about whether `$fn($p)` is a load of `T`
    //
    // `from` is nullable and `to` is not: that narrowing is the whole of why this is not
    // t_widening. the pointee comparison is the borrow arms', const rule included
    inline bool pointer_borrows_through(const ValueType &from, const ValueType &to)
    {
        return from.is_pointer() && from.is_nullable()
            && parameter_auto_borrows(to)
            && borrow_type_matches(from.pointee(), to);
    }

    // `AddrOf(Deref(arg))` - the wrap t_borrow_through scores. one helper so CallResolver and
    // PointerAdjuster cannot plant two different trees for one rank
    inline ExprNode *borrow_through_pointer(NodeCollection &nodes, ExprNode *arg)
    {
        assert(arg != nullptr && "borrow_through_pointer of a missing argument");
        return &nodes.emplace_back<AddrOfExprNode>(&nodes.emplace_back<DerefExprNode>(arg));
    }

    // `AddrOf(arg)` - the wrap the four fit_is_borrow ranks score. CallResolver plants it at an
    // argument; OwnershipPass plants the same node at a T& declaration or return, so a place
    // (`$a[0]`, `$i`) answers a local borrow the way it already answers a parameter
    inline ExprNode *borrow_place_if_wanted(NodeCollection &nodes, ExprNode *arg, ArgumentFit fit)
    {
        if (!fit_is_borrow(fit)) {
            return arg;
        }

        assert(arg != nullptr && "borrow_place_if_wanted of a missing argument");
        return &nodes.emplace_back<AddrOfExprNode>(arg);
    }

    // **what a `#[implicit]` conversion has to return to answer this parameter.**
    //
    // A borrow parameter is answered by a conversion to its *pointee*. The conversion produces a value,
    // and the borrow of that value is a separate and later rank, which is exactly why
    // t_borrow_temporary sits below t_declared_conversion. So the question is asked one level in.
    //
    // `const` is dropped for the borrow arms' reason: `const string::view&` is answered by the same
    // declaration a bare one is, and is_implicitly_convertible accepts the resulting `string::view&`
    // without a cast.
    //
    // Its own rule with two readers, rather than a peel spelled at each. AST::argument_fit decides
    // *that* a conversion applies and AST::CallResolver retrieves *which*, and a disagreement between
    // the two trips the `assert` in convert_if_wanted. AST::find_implicit_conversion itself is
    // deliberately left exact - its comparison is what keeps a chain of conversions unsearchable - so
    // the peel belongs here, where the thing being described is a parameter position
    inline ValueType implicit_conversion_target(const ValueType &param)
    {
        return parameter_auto_borrows(param) ? ValueType::make_mutable(param.pointee()) : param;
    }

    // **does the argument arrive through a borrow this conversion reads past?**
    //
    // a place or a call that returned a borrow, and **non-nullable only**. t_read_through ranks a
    // `ptr<T>` place as a copy of the pointee; converting through that place is a different question.
    // a `ptr<T>` place still answers read_reaches_storage - the slot is certainly there - so without
    // this conjunct a conversion would be found on the pointee and PointerAdjuster would then deref
    // `$p` with no null check.
    //
    // two readers that have to agree or the receiver is addressed twice: the source peel below,
    // and the const gate in implicit_conversion_for. `const` is dropped for the target's reason;
    // whether a const source may reach a non-const conversion is AST::const_receiver_refused
    inline bool implicit_conversion_peels_borrow(const ValueType &from, const ExprNode *expr)
    {
        return expr != nullptr
            && from.is_pointer()
            && !from.is_nullable()
            && read_reaches_storage(*expr, from);
    }

    inline ValueType implicit_conversion_source(const ValueType &from, const ExprNode *expr)
    {
        const ValueType source = implicit_conversion_peels_borrow(from, expr)
            ? value_type_of(from)
            : from;

        // a copy out of const storage is a mutable value, so `const uint64` reaches
        // `from(uint64)`. inbound compares parameter types exactly
        return ValueType::make_mutable(source);
    }

    // **which conversion answers this argument at this parameter, or none.** the two peels
    // plus the one thing neither can say: a peeled `const` borrow may only reach a
    // `const function` outbound conversion. asked of AST::const_receiver_refused, which owns that
    // question. inbound has no receiver; `const_receiver_refused` already answers false for a
    // static, so the gate is a no-op there rather than a second rule.
    //
    // **`from` no longer has to be a declared type.** that was what made `int32 → Quantity`
    // impossible: only the source's `_implicit_conversions` was searched. inbound lives on `to`,
    // and find_implicit_conversion asks both
    inline FunctionDeclNode *implicit_conversion_for(
        const ValueType &from,
        const ExprNode *expr,
        const ValueType &to
    )
    {
        const ValueType source = implicit_conversion_source(from, expr);

        FunctionDeclNode *conversion =
            find_implicit_conversion(source, implicit_conversion_target(to));

        if (conversion == nullptr) {
            return nullptr;
        }

        // only when a borrow was actually peeled: a value argument has no const-ness of its own
        // to launder, and the temporary the conversion's receiver binds to is the compiler's.
        // inbound is a static, so const_receiver_refused is already false for it
        if (implicit_conversion_peels_borrow(from, expr)
            && const_receiver_refused(*conversion, from)) {
            return nullptr;
        }

        return conversion;
    }

    // **the** rule for "does this argument answer this parameter" - the only implementation, with
    // three readers that each take a different amount of it:
    //
    //  - AST::match_function ranks with the whole ordering, which is what picks an overload;
    //  - AST::CallResolver's argument coercion, and OwnershipPass at a T& declaration or
    //    return, read `fit_is_borrow`, so a place the matcher accepted is a place the wrap
    //    then actually addresses;
    //  - AST::TypeChecker reads only `!= t_none`, so a call it reports is a call resolution could
    //    not have chosen.
    //
    // `expr` may be null when only the argument's type is known. The borrow rule needs the expression,
    // because only an expression can be a place, so a caller that passes null declines the borrow arm.
    // That is what a *cast* wants: a cast is not an address-of
    inline ArgumentFit argument_fit(const ValueType &from, const ExprNode *expr, const ValueType &to)
    {
        // void produces no value. a concrete destination is a mismatch; an unbound T stays
        // undetermined below so the call waits and rewrite_value_edge names the void call
        if (from.is_void() && !to.is_void() && !is_undetermined_type(to)) {
            return ArgumentFit::t_none;
        }

        // **a written `null` is untyped, but it is not unconstrained.** it can only ever land in a
        // destination that admits absence, so a determined parameter that does not is a mismatch
        // now, not a question for a later round. without this, `f(int64?)` beside `f(string)` was
        // undecidable for `f(null)` - the `string` overload "remained possible" for a value it can
        // never hold. an undetermined parameter (a bare `T`) still waits below
        if (is_written_null(expr) && type_is_determined_for_fit(to) && !destination_admits_null(to)) {
            return ArgumentFit::t_none;
        }

        // no information. checked so an undetermined argument can never be read as a
        // mismatch - the type checker and the monomorphizer's inference both already treat
        // unknown this way. void vs int32 is a mismatch (above). AST::type_is_determined_for_fit
        // is the one wait: a match binding declared `unknown&` is still a placeholder
        if (!type_is_determined_for_fit(from) || !type_is_determined_for_fit(to)) {
            return ArgumentFit::t_undetermined;
        }

        if (from == to) {
            return ArgumentFit::t_exact;
        }

        // `&name` is a C pointer until bind_function_ref_to flips it. a function<...>
        // parameter scores the Echo callable of that signature - AST::function_ref_as
        // is the one rewrite, and Instantiation asks it too
        {
            const ValueType as_callable = function_ref_as(from, to);

            if (as_callable != from) {
                return argument_fit(as_callable, nullptr, to);
            }
        }

        // **a tagged optional accepts whatever its payload accepts.** `f($circle)` against a `Drawable?`
        // parameter widens twice - to the interface, then into the absence - and every arm below asks about
        // one step. asked here, once, so the answer cannot differ per arm: the rank is the payload's,
        // because the wrapping is a coerce_value AST::CallResolver inserts and costs the caller nothing.
        // wrapping vs not wrapping at the same payload rank is match_function's second key, so
        // `g(string)` beats `g(string?)` for `'a'` without a rank that would also make wrap-of-promotion
        // beat a plain promotion
        //
        // an argument that is *already* nullable is left to the arms below, where the equality above has
        // already accepted the one shape that fits - which is AST::arrival_wraps_optional's second half,
        // and asked through it because the cast AST::CallResolver mints and the `insertvalue` pair
        // TypeLowering::coerce_value emits are the same decision made twice more
        if (arrival_wraps_optional(from, to)) {
            return argument_fit(from, expr, to.optional_payload());
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

        // reading one level *through* a borrow to fill a value parameter, which is what `take($b)`
        // means for a `Point& $b` and a `Point` parameter. The deref is inserted afterwards by
        // PointerAdjuster::as_value_for, exactly as the borrow arm's address-of is inserted by
        // CallResolver - so both arms score a wrapping that a later pass performs.
        //
        // AST::unify_type already decays a pointer argument to its pointee for a value parameter (its
        // `allow_decay` arm), so without this rank inference named an instance that scoring then
        // rejected. With one candidate the disagreement was invisible, because matching rule 2 wins
        // without consulting types at all. With two it was a hard "no overload accepts these
        // arguments", so adding an unrelated overload broke a call that had always compiled.
        //
        // asked of AST::read_peels_pointer, so a **call** returning a borrow ranks here too -
        // `f($a->at(0))` at an `int32` parameter. a call returning a `ptr<T>` does not: that
        // predicate is non-nullable for calls, which is the line that keeps
        // `if (c_getenv($name) == null)` from becoming a comparison of `uint8`. a `ptr<T>` *place*
        // still ranks, because the slot is there - ranking it is how `f(T)` beats `f(T&)` for `$p`
        // - unless the pointee is incomplete, in which case the pointer *is* the value
        if (!to.is_pointer() && expr != nullptr && read_peels_pointer(*expr, from)) {
            if (ValueType::make_mutable(value_type_of(from)) == ValueType::make_mutable(to)) {
                return ArgumentFit::t_read_through;
            }
        }

        // a `ptr<T>` place at a `T&` parameter. after t_read_through, which is the *value*
        // destination of the same peel, and after the ordinary borrow arm, which is a `T` place
        // already. one level, and a place - the same read_reaches_storage t_read_through asks. a call
        // that returned a `ptr<T>` stays out of both ranks: there is no slot to peel from
        if (pointer_borrows_through(from, to)
            && expr != nullptr && read_reaches_storage(*expr, from)) {
            return ArgumentFit::t_borrow_through;
        }

        // whatever is left between two primitives is a conversion TypeLowering::coerce_value
        // knows how to emit. a pointer, struct or class on either side has no such table
        if (from.is_primitive() && to.is_primitive() && !from.is_void() && !to.is_void()) {
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

        // `#[implicit]` conversion, last of the real ranks. asked of implicit_conversion_target so a
        // borrow peels to what it borrows. a non-place gets a slot the same way every other borrow does
        if (expr != nullptr && implicit_conversion_for(from, expr, to) != nullptr) {
            return ArgumentFit::t_declared_conversion;
        }

        // a borrow parameter filled by a value with no storage: mint a slot. last arm, not last rank
        // (it still out-ranks the conversions above). `from` must not itself be a pointer
        if (parameter_auto_borrows(to) && expr != nullptr && !from.is_pointer()
            && can_bind_temporary(*expr) && borrow_type_matches(from, to)) {
            return borrow_rank(
                from, to, ArgumentFit::t_borrow_temporary, ArgumentFit::t_borrow_temporary_const);
        }

        return ArgumentFit::t_none;
    }
};

#endif
