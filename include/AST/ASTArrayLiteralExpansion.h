#ifndef ASTARRAYLITERALEXPANSION_H
#define ASTARRAYLITERALEXPANSION_H

#pragma once

#include "AST/ASTCodeRef.h"
#include "AST/ASTNode.h"
#include "AST/ASTValueType.h"

#include <vector>

namespace AST
{
    class ArrayLiteralExprNode;
    class Collector;
    class ExprNode;
    class File;
    class FunctionCallExprNode;
    class Module;
    class Namespace;
    class ScopeNode;
    class VarDeclNode;

    // **turns `[1, 2, 3]` into a constructor plus one append per element.**
    //
    // AST::OperatorRewriter is the walk and the placement. this is the rewrite: which statement owns
    // the literal, whether that storage can be filled in place, and how to mint a hoist when it cannot.
    // split out because the rewriter already holds five other rules and this one is several statements
    // and two lifetimes.
    //
    // **one predicate for in-place vs hoist:** the destination is mutable variable storage. a const
    // declaration, an assignment through `$a[]` or `$s->items`, and an argument all hoist. what differs
    // is only where the walk places the statements this returns - siblings of a declaration so the
    // name outlives the temp, a wrapper around anything else so the temp dies with the statement
    class ArrayLiteralExpansion
    {
    public:
        ArrayLiteralExpansion(
            Module &module,
            Collector &collector,
            File *file,
            bool finalizing,
            size_t &hoist_count
        );

        // the two statement shapes an array literal may sit in, resolved to the storage being filled
        // and the literal filling it. `decl` null with `slot` set is an assignment whose target is
        // not a variable. `slot` null means the statement is not one of them
        struct Destination
        {
            VarDeclNode *decl = nullptr;
            ArrayLiteralExprNode *literal = nullptr;
            ExprNode **slot = nullptr;

            // is this statement the declaration itself? **only a declaration may be typed *from* its
            // elements** - an assignment writes storage somebody else already named
            bool declares = false;
        };

        static Destination destination_of(Node *statement);

        // a statement-level literal. in-place fills `slot` and splices appends after `index`.
        // otherwise the hoist statements go into `hoisted` and `slot` becomes `mv $__lit`.
        // answers whether the tree changed
        bool expand_statement(ScopeNode &scope, size_t index, std::vector<NodeReference> &hoisted);

        // an expression-position literal. the hoist statements go into `hoisted`; the replacement
        // is the temp's name, or null when the literal is not ready. `hoist_barrier` is how deep
        // inside `??` / `?->` the walk is
        ExprNode *expand_expression(
            ArrayLiteralExprNode &literal,
            std::vector<NodeReference> &hoisted,
            size_t hoist_barrier
        );

        void report_unplaced(ArrayLiteralExprNode &literal);

    private:
        bool settle_destination_type(const Destination &destination, ValueType &settled);

        bool bind_unplaced_type(ArrayLiteralExprNode &literal);

        bool build_expansion(
            ArrayLiteralExprNode &literal,
            VarDeclNode &into,
            const ValueType &type,
            ExprNode **slot,
            std::vector<NodeReference> &appends
        );

        // answers the temp's name, having appended the declaration and its appends to `hoisted`.
        // null when the literal is not ready, or when a hoist would move construction above a branch
        ExprNode *hoist(
            ArrayLiteralExprNode &literal,
            std::vector<NodeReference> &hoisted,
            size_t hoist_barrier
        );

        FunctionCallExprNode &build_constructor_call(
            const std::string &name,
            const TokenReference &at,
            const Namespace *lookup
        );

        CodeRef code_ref_for(const TokenReference &token);

        Module &_module;
        Collector &_collector;
        File *_file;
        bool _finalizing;
        size_t &_hoist_count;
    };

    // places `hoisted` ahead of `scope.children[index]`. a declaration is spliced as a sibling so
    // the name it introduces outlives the temp; every other statement is wrapped so the temp dies
    // with the statement rather than the frame
    void place_array_literal_hoists(
        ScopeNode &scope,
        size_t index,
        Module &module,
        std::vector<NodeReference> &hoisted
    );
};

#endif
