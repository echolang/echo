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
        // the source conforms to `contract::iterable<V>`: call its `iterate()`
        t_iterable,

        // the source *is* a `contract::iterator<V>`: drive it directly. this is what makes an adaptor - a
        // filter, a map - an ordinary struct rather than a language feature
        t_iterator,

        // an erased interface value that is a `contract::iterator<V>` application. the same shape as above,
        // with the two calls dispatched through the vtable, and it costs the lowering nothing to permit:
        // find_member_functions finds a requirement in the same `_methods` list, and gen_function_call
        // already routes on FunctionDeclNode::is_interface_requirement()
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

        // V - the element type, carrying its own const
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
