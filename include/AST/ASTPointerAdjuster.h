#ifndef ASTPOINTERADJUSTER_H
#define ASTPOINTERADJUSTER_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ASTIssue.h"

namespace AST
{
    class Bundle;
    class Collector;
    class Module;
    class File;
    class ExprNode;

    // inserts the auto-deref that a pointer's transparency implies, explicitly, into the tree.
    //
    // a pointer "behaves like the value it points to", and codegen has always honoured that by
    // emitting a second load when it read a pointer variable. the AST did not: a read of
    // `int32& $x` reported its type as `int32&` while the value produced was an `int32`, and
    // every consumer downstream - binary operand typing, the echo format dispatch, argument
    // matching, generic inference - had to reach for value_type_of() to paper over it.
    //
    // this pass wraps every pointer read in a value position with a DerefExprNode, so after it
    // runs `result_type()` means what it says and nobody has to compensate.
    //
    // it is a *rewriter*, not a walker: it replaces the parent's edge to a child. that is why
    // it drives the traversal itself through adjust() rather than subclassing the read-only
    // RecursiveVisitor's descent.
    //
    // runs after monomorphization - a type parameter's pointer-ness is not known until it is
    // substituted - and before the type checker, which can then compare types structurally
    class PointerAdjuster
    {
    public:
        PointerAdjuster(Bundle &bundle);

        void run();

    private:
        Bundle &_bundle;
        Collector &_collector;

        // the module whose file is currently being walked; new deref nodes are emplaced into
        // its collection, which is the sole owner of every node in that module
        Module *_current_module = nullptr;
        File *_current_file = nullptr;

        // the function whose body is currently being walked, so a return knows what the value
        // has to fit. null at file scope, where a return has no declared type to answer to
        FunctionDeclNode *_current_function = nullptr;

        // walks the node, rewriting each of its expression edges through as_value()
        void adjust(Node *node);

        // the value-position form of an expression: a pointer read gains one deref, everything
        // else is returned unchanged. null-safe
        ExprNode *as_value(ExprNode *expr);

        // adjust the expression subtree *without* making its root a value - used for the
        // operand of an address-of, and for a member access base, both of which want the place.
        // returns the possibly-replaced expression
        ExprNode *adjust_place(ExprNode *expr);

        // as_value, except that a pointer-shaped destination keeps the address rather than
        // reading through it. shared by declarations and assignments so the two agree
        ExprNode *as_value_for(ExprNode *expr, const ValueType &wanted);

        // erases a `:$` marker, leaving the operand as a place whose value is the pointer.
        // reports here, where the type is finally concrete, when there is nothing to peel
        ExprNode *strip_peel(ExprNode *expr);

        // gives an untyped `null` the type of the operand it is being compared against
        void bind_null_operand(ExprNode *maybe_null, ExprNode *other);

        CodeRef code_ref_for(const TokenReference &token);
    };
};

#endif
