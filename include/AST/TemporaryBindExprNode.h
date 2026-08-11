#ifndef TEMPORARYBINDEXPRNODE_H
#define TEMPORARYBINDEXPRNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ASTNodeReference.h"
#include "AST/ExprNode.h"
#include "Token.h"

#include <vector>

namespace AST
{
    class VarDeclNode;

    // **a value nobody stored, given storage for exactly as long as one expression reads it.**
    //
    // `$o->mid()->tag` reads a member off a call result. a member is reached from an address and a value
    // nobody stored has none, so it needs a slot - and the slot needs an *owner*, because
    // `$box->get()` hands back +1 for a class and reading one field off it and walking away leaks. that
    // owner cannot be the enclosing statement, and that is the whole reason this is a node rather than
    // a hoist: binding at the top of the statement would acquire a `while` condition's temporary once
    // per evaluation and release it once in total, and would bind the right-hand side of a
    // short-circuiting `&&` on the path that never evaluates it. a temporary has to live where it is
    // *used*, and only an expression knows where that is
    //
    // bind `temporaries`, evaluate `body`, run `teardown`, hand back the body's value. every property
    // of this node follows from those four being in that order: the body has already read what it
    // wanted out of the temporary before the teardown runs, which is why a class-typed member arrives
    // here already wrapped in a RetainExprNode - AST::OwnershipPass put it there through the ordinary
    // "a place is copied" arm, with no arm of its own - and is one reference up before the teardown's
    // release takes one back down
    //
    // **the value handed out is one nobody else holds**, which is the invariant every non-place in this
    // compiler carries and what lets a call result arrive anywhere with nothing inserted. the retain
    // above is how that invariant is kept for a class read out of the temporary; a value the compiler
    // has no copy rule for is a located error instead, worded where the copy rules are
    //
    // **deliberately not a place** - AST::is_place_expression has no arm for it and must not grow one.
    // its value is a copy of something about to be destroyed, so `&$o->mid()->x` would hand out an
    // address that dangles at the end of the statement and `$o->mid()->x = 5` would write into bytes
    // nothing will ever read. both stay located errors from AST::OwnershipPass, which is the only pass
    // that ever knew a temporary was wanted
    class TemporaryBindExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_temp_bind);

        // the temporaries this expression owns, in binding order. exactly one today: a temporary is
        // needed for the *root* of a place chain, and a chain has one root. a list because the next
        // producer binds around a whole call, where two arguments want two
        //
        // owned children rather than locals of the enclosing scope: a frame drops its locals in reverse
        // declaration order at the scope's *end*, and for a temporary inside a condition neither the
        // position nor the count is right
        std::vector<VarDeclNode *> temporaries;

        // the expression the temporaries were bound for. its value is this node's value, which is also
        // why result_type() has nothing of its own to say
        ExprNode *body = nullptr;

        // the drops, in the order they run - reverse binding order, through the same emit_drop a
        // scope's end uses. empty is the common case and the correct one: a struct that owns nothing
        // owes nothing
        //
        // AssignNode::teardown_old's shape for its reason - the drops are ordinary calls in the tree,
        // so -ar shows them, AST::TypeChecker validates them, and a generic teardown named by one is
        // instantiated by the next round of the very fixpoint that inserted it
        NodeReferenceList teardown;

        // the member name that asked for the temporary. every diagnostic about this node points there,
        // which is where the "has no storage to read a member from" it replaces pointed
        TokenReference token;

        TemporaryBindExprNode(ExprNode *body, const TokenReference &token) :
            body(body), token(token)
        {
            assert(body != nullptr && "TemporaryBindExprNode requires a body");
        };

        ~TemporaryBindExprNode() {};

        ValueType result_type() const override;

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_temporary_bind(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
