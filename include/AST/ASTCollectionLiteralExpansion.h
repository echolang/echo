#ifndef ASTCOLLECTIONLITERALEXPANSION_H
#define ASTCOLLECTIONLITERALEXPANSION_H

#pragma once

#include "AST/ASTCodeRef.h"
#include "AST/ASTNode.h"
#include "AST/ASTValueType.h"

#include <vector>

namespace AST
{
    class ArrayLiteralExprNode;
    class CollectionLiteralExprNode;
    struct CollectionFill;
    class Collector;
    class ComplexType;
    class DetachBatch;
    class ExprNode;
    class File;
    class FunctionCallExprNode;
    class Module;
    class Namespace;
    class ScopeNode;
    class VarDeclNode;

    // **turns `[1, 2, 3]` into a constructor plus one append per element, and `["k" => v]`
    // into a constructor plus one `$dest[k] = v` per entry.** the placement is one rule;
    // a second copy of the hoist is how a temp outlives a statement.
    //
    // a long integer table is one `arr::extend_from` of a constant global: fill_from_constant_data.
    //
    // AST::OperatorRewriter is the walk and the placement. this is the rewrite: which statement owns
    // the literal, whether that storage can be filled in place, and how to mint a hoist when it cannot.
    // split out because the rewriter already holds five other rules and this one is several statements
    // and two lifetimes.
    //
    // **one predicate for in-place vs hoist:** the destination is mutable variable storage. a const
    // declaration, an assignment through `$a[]` or `$s->items`, and an argument all hoist. what differs
    // is only where the walk places the statements this returns: siblings of a declaration or a
    // `guard` so the name outlives the temp, a wrapper around anything else so the temp dies with
    // the statement
    class CollectionLiteralExpansion
    {
    public:
        CollectionLiteralExpansion(
            Module &module,
            Collector &collector,
            File *file,
            bool finalizing,
            size_t &hoist_count,
            DetachBatch &detached
        );

        // the two statement shapes a collection literal may sit in, resolved to the storage being
        // filled and the literal filling it. `decl` null with `slot` set is an assignment whose
        // target is not a variable. `slot` null means the statement is not one of them
        struct Destination
        {
            VarDeclNode *decl = nullptr;
            CollectionLiteralExprNode *literal = nullptr;
            ExprNode **slot = nullptr;

            // is this statement the declaration itself? **only a declaration may be typed *from* its
            // entries** - an assignment writes storage somebody else already named
            bool declares = false;
        };

        static Destination destination_of(Node *statement);

        // a statement-level literal. in-place fills `slot` and splices appends after `index`.
        // otherwise the hoist statements go into `hoisted` and `slot` becomes `mv $__lit`.
        // answers whether the tree changed
        bool expand_statement(ScopeNode &scope, size_t index, std::vector<NodeReference> &hoisted);

        // an expression-position literal. the hoist statements go into `hoisted`; the replacement
        // is the temp's name, or null when the literal is not ready
        ExprNode *expand_expression(
            ExprNode *expr,
            std::vector<NodeReference> &hoisted
        );

        void report_unplaced(CollectionLiteralExprNode &literal);

    private:
        bool settle_destination_type(const Destination &destination, ValueType &settled);

        bool bind_unplaced_type(CollectionLiteralExprNode &literal);

        // classify once via collection_fill_of, mint the constructor (unless inline), then emit
        // appends, keyed writes, indexed writes, or nothing
        bool build_expansion(
            CollectionLiteralExprNode &literal,
            VarDeclNode &into,
            const ValueType &type,
            ExprNode **slot,
            std::vector<NodeReference> &appends
        );

        bool mint_constructor(
            CollectionLiteralExprNode &literal,
            const ValueType &type,
            ExprNode **slot
        );

        void emit_index_assign(
            CollectionLiteralExprNode &literal,
            VarDeclNode &into,
            std::vector<ExprNode *> indices,
            ExprNode *value,
            std::vector<NodeReference> &appends
        );

        // the memcpy path: one `arr::extend_from`. false: walk the appends
        bool fill_from_constant_data(
            ArrayLiteralExprNode &literal,
            VarDeclNode &into,
            const ValueType &type,
            const CollectionFill &fill,
            std::vector<NodeReference> &appends
        );

        // answers the temp's name, having appended the declaration and its appends to `hoisted`.
        // null when the literal is not ready
        ExprNode *hoist(
            CollectionLiteralExprNode &literal,
            std::vector<NodeReference> &hoisted
        );

        FunctionCallExprNode &build_named_call(
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
        DetachBatch &_detached;
    };

    // places `hoisted` ahead of `scope.children[index]`. a declaration **or a `guard`** is spliced
    // as a sibling so the binding outlives the temp; wrapping would drop it with the hoist.
    // every other statement is wrapped so the temp dies with the statement
    void place_collection_literal_hoists(
        ScopeNode &scope,
        size_t index,
        Module &module,
        std::vector<NodeReference> &hoisted
    );
};

#endif
