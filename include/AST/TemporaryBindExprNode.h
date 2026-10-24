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

    // a value nobody stored, given a slot for as long as one expression reads it.
    // `$o->mid()->tag` needs an address, and a hoist to the statement would bind a `while`
    // condition once and a short-circuit `&&` on the path that never runs it. not a place:
    // `&$o->mid()->x` would dangle at the end of the statement
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

        // statements that fill those temporaries, after they are seated and before the body reads
        // them. a match arm is an expression, so collection-literal appends cannot be siblings of a statement
        NodeReferenceList statements;

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
