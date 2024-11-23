#ifndef ASSIGNNODE_H
#define ASSIGNNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ExprNode.h"
#include "Lexer.h"

namespace AST
{
    // an assignment statement: `<place> = <value>`.
    //
    // one node for every left hand side shape. `$x = e`, `$s->f = e` and every place expression
    // added later resolve through the same lvalue path in codegen, so a new spelling on the left
    // costs nothing here. the previous design had one node per shape (VarMutNode, MemberMutNode),
    // each with its own codegen and its own copy of the numeric coercion cascade, and every new
    // left hand side form would have added a third and a fourth.
    //
    // still a statement, not an expression: `$a = $b = 1` remains unrepresentable, which is what
    // the scope parser expects
    class AssignNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_assign);

        // must be a place expression - see AST::is_place_expression
        ExprNode *target;
        ExprNode *value_expr;

        // the `=` token, so a mismatched assignment reports at the assignment
        TokenReference token_assign;

        // an initialization binds storage for the first time rather than mutating it, so a const
        // destination is legal here. only the synthesized struct constructor sets it - a `const`
        // property is written exactly once, by the initializer the parser writes for it, and the
        // const checks in AST::TypeChecker would otherwise reject that write along with real ones
        bool is_initialization = false;

        // the target is a class-typed place, so whatever reference it held before this write has to be
        // released. set by AST::OwnershipPass, which is where every other ownership decision is made -
        // but the *sequence* is codegen's, because the old handle only exists at runtime and there is
        // nowhere to put it once the store has happened. gen_assign therefore reads the old handle out
        // of the slot before storing and releases it after, which is what makes `$a = $a` safe: the
        // retain on the right-hand side has already run by then.
        //
        // false when the variable was moved out of - the reference it appears to hold is somebody
        // else's now
        bool releases_old = false;

        AssignNode(ExprNode *target, ExprNode *value_expr, TokenReference token_assign)
            : target(target), value_expr(value_expr), token_assign(token_assign)
        {
            assert(target != nullptr && "Assignment target cannot be null");
        };

        ~AssignNode() {};

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_assign(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
