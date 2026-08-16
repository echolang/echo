#ifndef ASTPOINTERADJUSTER_H
#define ASTPOINTERADJUSTER_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ASTIssue.h"
#include "AST/ASTRecursiveVisitor.h"

namespace AST
{
    class Bundle;
    class Collector;
    class Module;
    class File;
    class ExprNode;

    // inserts the auto-deref that a pointer's transparency implies, explicitly, into the tree
    //
    // a pointer "behaves like the value it points to", and codegen has always honoured that by
    // emitting a second load when it read a pointer variable. the AST did not: a read of
    // `int32& $x` reported its type as `int32&` while the value produced was an `int32`, and
    // every consumer downstream - binary operand typing, the echo format dispatch, argument
    // matching, generic inference - had to reach for value_type_of() to paper over it
    //
    // this pass wraps every pointer read in a value position with a DerefExprNode, so after it
    // runs `result_type()` means what it says and nobody has to compensate
    //
    // it is a *rewriter*: it replaces the parent's edge to a child rather than only reading it.
    // AST::RecursiveVisitor owns the descent, and rewrite_value_edge / rewrite_place_edge are the
    // two seams that make a walker a rewriter. a switch over NodeType ending in a `default:` that
    // treats an unknown tag as a leaf would silently skip a `guard`, a `??`, a `?->` or a `strong`
    //
    // **what stayed here is the destination type**, and only that: the six positions below where a
    // value is read *toward* something. the destination comes from a sibling field, a cross-reference
    // or this pass's own function stack, none of which a generic walker could answer - so hoisting it
    // would be a second answer to "how far is a value read", which is this file's whole question
    //
    // runs after monomorphization - a type parameter's pointer-ness is not known until it is
    // substituted - and before the type checker, which can then compare types structurally
    class PointerAdjuster : public RecursiveVisitor
    {
    public:
        PointerAdjuster(Bundle &bundle);

        void run();

        // the six positions that read a value *toward a type*. everything else is the base's descent
        // plus as_value(), which is what rewrite_value_edge below hands it
        void visitVarDecl(VarDeclNode &node) override;
        void visit_const_decl(ConstDeclNode &node) override;
        void visit_const_ref(ConstRefExprNode &node) override;
        void visit_assign(AssignNode &node) override;
        void visitFunctionCallExpr(FunctionCallExprNode &node) override;
        void visit_indirect_call_expr(IndirectCallExprNode &node) override;
        void visit_closure_expr(ClosureExprNode &node) override;
        void visitReturn(ReturnNode &node) override;

        // and the four that are not about a destination at all
        void visitFunctionDecl(FunctionDeclNode &node) override;
        void visitBinaryExpr(BinaryExprNode &node) override;
        void visit_release(ReleaseNode &node) override;
        void visit_const_if(ConstIfNode &node) override;
        void visit_const_expr(ConstExprNode &node) override;
        void visit_foreach(ForeachNode &node) override;
        void visit_string_interpolation(StringInterpolationExprNode &node) override;

    protected:
        ExprNode *rewrite_value_edge(ExprNode *expr) override;
        ExprNode *rewrite_place_edge(ExprNode *expr) override;

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

        // the value-position form of an expression: descend into it, then a pointer read gains one
        // deref and everything else is returned unchanged. null-safe. **this is rewrite_value_edge** -
        // named separately because the three destination-aware arms call it by this name, and because
        // "as a value" is the vocabulary the rest of the file and the spec are written in
        ExprNode *as_value(ExprNode *expr);

        // the same descent *without* making the root a value - the operand of an address-of, a member
        // access base, an index base. **this is rewrite_place_edge**, and the base decides which
        // positions are ones. returns the possibly-replaced expression
        ExprNode *adjust_place(ExprNode *expr);

        // as_value, except that a pointer-shaped destination keeps the address rather than
        // reading through it. shared by declarations and assignments so the two agree
        ExprNode *as_value_for(ExprNode *expr, const ValueType &wanted);

        // a call's arguments, each read as far as its own parameter wants. `wanted_at` answers
        // the parameter type at an index - off the declaration for a direct call, off the
        // callee's signature for an indirect one, which is the only difference between the two
        template <typename WantedAt>
        void adjust_call_arguments(std::vector<ExprNode *> &arguments, WantedAt wanted_at);

        // erases a `:$` marker, leaving the operand as a place whose value is the pointer
        // reports here, where the type is finally concrete, when there is nothing to peel
        ExprNode *strip_peel(ExprNode *expr);

        // gives an untyped `null` the type of the operand it is being compared against
        void bind_null_operand(ExprNode *maybe_null, ExprNode *other);

        CodeRef code_ref_for(const TokenReference &token);
    };
};

#endif
