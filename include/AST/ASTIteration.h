#ifndef ASTITERATION_H
#define ASTITERATION_H

#pragma once

#include "AST/ASTValueType.h"

#include <optional>
#include <string>

namespace AST
{
    class CoreTypes;
    class FunctionDeclNode;

    // where the cursor a loop drives comes from
    enum class IterationSource
    {
        // the source conforms to `contract::iterable<V>` - or, when the loop was handed a const value,
        // to `contract::const_iterable<V>`: call its `iterate()`. **one kind and not two**, because
        // which contract was read changes nothing after the plan is built: the callee is named through
        // whichever conformance answered, and V arrives already carrying the const the const one spells
        t_iterable,

        // the source *is* a `contract::iterator<V>`: drive it directly. this is what makes an adaptor - a
        // filter, a map - an ordinary struct rather than a language feature
        t_iterator,

        // an erased interface value that is a `contract::iterator<V>` application. the same shape as above,
        // with the two calls dispatched through the vtable, and it costs the lowering nothing to permit:
        // find_member_functions finds a requirement in the same `_methods` list, and gen_function_call
        // already routes on FunctionDeclNode::is_interface_requirement()
        //
        // **that was true of the lowering and not of the dispatch, and for a long time nothing said so.**
        // this arm had no corpus case, and a call through an erased *generic* interface had no vtable
        // slot at all - AST::interface_method_slot matched the requirement by pointer identity against
        // the template's methods while the call carries the instantiation's - so every program that
        // reached here died with an internal error. the unwrapping protocol had the identical arm.
        // `tests_eco/iteration/erased_iterator` is this one, and it exists now
        t_erased_iterator,
    };

    struct IterationPlan
    {
        IterationSource kind = IterationSource::t_iterable;

        // the `iterate()` that produces the cursor. null for the two iterator kinds, where the source
        // already *is* one
        FunctionDeclNode *iterate = nullptr;

        // the cursor's type, once known. for t_iterable it is the associated `Iter` binding; for the two
        // iterator kinds it is the source itself
        ValueType iterator_type;

        // V - the element type, **carrying its own const**, which is load-bearing rather than incidental:
        // it is the entire difference a const source makes to the loop. a container declares
        // `const_iterable<const T>`, V is read off that application, and both binding rules downstream
        // fall out of it with no arm of their own - `&$el` over a const V is AST::ForeachLowering's
        // refusal, `const &$el` and the elided by-value form are its `const V&` declaration
        ValueType element_type;

        // K, when the cursor declares `contract::keyed<K>`. absent is not an error until a `=>` asks for it
        std::optional<ValueType> key_type;

        // **no `yield` field.** `current()` hands back a borrow, full stop - that is what the protocol
        // declares, and AST::ForeachLowering reads it that way. a future `ValueIterator<V>` would add
        // the field *and* the arm at the binding site that reads it; a field nothing writes today would
        // only read as a decision this owner makes
    };

    // **the sole answer to "how is this value iterated".**
    //
    // three results and not a bool, for AST::can_instantiate's reason: `t_pending` is what a source
    // whose type the fixpoint has not settled yet must get, and it has to be distinguishable from a
    // refusal - otherwise the loop reports "'[unknown]' cannot be iterated" once per round and the
    // first round's guess becomes the diagnostic.
    //
    // reports nothing itself. it has no CodeRef, and the caller does - the same split
    // AST::interface_erasure_refusal makes, and for the same reason
    struct IterationLookup
    {
        enum class Result
        {
            t_ok,

            // ask again next round: the source's type, or the cursor's, is not settled yet
            t_pending,

            // `refusal` is a whole sentence, phrased for the author of the loop
            t_refused,
        };

        Result result = Result::t_pending;
        IterationPlan plan;
        std::string refusal;
    };

    IterationLookup iteration_plan_for(const ValueType &source, const CoreTypes &core, TypeRegistry &types);
};

#endif
