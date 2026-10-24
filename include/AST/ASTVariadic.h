#ifndef ASTVARIADIC_H
#define ASTVARIADIC_H

#pragma once

#include "AST/ASTValueType.h"

#include <optional>
#include <string>

namespace AST
{
    class ArrayLiteralExprNode;
    class CoreTypes;
    class ExprNode;
    class FunctionDeclNode;

    // **is this the C variadic marker?** `#[echoc::core: variadic_args]`, with `const` and borrowing
    // ignored - the question is which type was named, and a `const variadic_args&` is as much a
    // refusal as a plain one everywhere but the parameter it is legal in
    bool is_variadic_args(const ValueType &type, const CoreTypes &core);

    // **is this expression the list that stands for a C variadic tail?** the literal when it is, null
    // otherwise - so a caller that wants the elements gets them in one step.
    //
    // one question with four askers, none of which may spell it for itself: `argument_fit` waits on the
    // elements rather than on the list, `CallResolver` promotes them, `TypeChecker` refuses the shapes C
    // cannot carry and codegen writes them out after the fixed arguments. `OperatorRewriter` is the
    // fifth, and it asks in the negative - a pack is the one bracket it must leave alone
    //
    // `PointerAdjuster` is the sixth, asking inside `adjust_call_arguments` - a pack is only ever a call
    // argument, so it is one more answer to "how far is a call argument read" rather than a position of
    // its own. it decides what an element *means*: a tail has no parameter opposite it, so each element
    // is its own destination and a `ptr<T>` place in one keeps its address instead of collecting the
    // deref a destinationless value edge would give it
    ArrayLiteralExprNode *variadic_pack_of(ExprNode *expr);

    // **does this declaration end in a C variadic tail?** the whole of what makes a call to it
    // variadic - there is no `...` in the grammar and no flag on the declaration, the last
    // parameter's *type* is the statement
    bool has_variadic_tail(const FunctionDeclNode &decl, const CoreTypes &core);

    // **why may a `variadic_args` not sit where it was written?** nullopt when it may.
    //
    // asked once, of every declaration, by AST::TypeChecker - rather than at each of the several
    // places a type gets bound to something. one sweep is what keeps `variadic_args $x;` inside a
    // function body and `function f() : variadic_args` refused by the same sentence-writer as the
    // parameter cases, instead of by whichever parser happened to have an arm
    std::optional<std::string> variadic_args_refusal(const FunctionDeclNode &decl, const CoreTypes &core);

    // C's default argument promotions. CallResolver is the only thing that coerces an argument.
    // float -> float64; bool / int8 / int16 -> int32; uint8 / uint16 -> uint32. the IR must show
    // the `fpext` / `sext`: a target's backend may promote the tail itself, so program output
    // does not catch a missing one
    ValueType variadic_promotion_of(const ValueType &type);

    // **may this value be a member of a variadic tail?** nullopt when it may.
    //
    // primitives and addresses, and nothing else: a struct passed by C's variadic convention is a
    // platform-specific unpacking no declaration here describes, and an interface or a callable is
    // two words with no C spelling at all
    std::optional<std::string> variadic_argument_refusal(const ValueType &type);
};

#endif
