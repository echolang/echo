#ifndef ASSIGNNODE_H
#define ASSIGNNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ExprNode.h"
#include "Lexer.h"

namespace AST
{
    class ScopeNode;

    // an assignment statement: `<place> = <value>`
    //
    // one node for every left hand side shape. `$x = e`, `$s->f = e` and every place expression
    // added later resolve through the same lvalue path in codegen, so a new spelling on the left
    // costs nothing here
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

        // an initialization binds storage for the first time rather than mutating it. two things
        // follow, and they are the whole meaning: a `const` destination is legal, because this is the
        // one write a `const` property is entitled to; and the old value is owed no teardown, because
        // there is no old value - the storage is a constructor's `$this`, which gen_var_decl
        // zero-fills
        //
        // set for the synthesized field-wise constructor's writes and for a hand-written
        // constructor's writes to its own `$this` (Context::ctor_this_ptr). those are the same
        // question - is this storage fresh - asked of code the compiler wrote and code the author
        // wrote, and it would be strange for the answer to depend on which
        bool is_initialization = false;

        // the value arriving here is handed over rather than copied, and no `mv` says so because
        // nobody wrote this statement. **only** the synthesized field-wise constructor sets it: its
        // parameter was given to it to be built into the struct, there is nowhere to put a `mv`, and
        // the constructor is the only thing that ever writes that field
        //
        // deliberately separate from is_initialization above. a *hand-written* constructor writes
        // fresh storage too, but its transfers are visible and must stay so - it says
        // `$this->data = mv $data`. folding the two would make `$this->inner = $other->inner`
        // silently move, and move out of a *borrowed* source at that
        bool hands_over_value = false;

        // --- the old value's teardown ------------------------------------------------------------
        //
        // the target held a value before this write, and that value is owed its end. AST::OwnershipPass
        // decides *whether*, gen_assign decides *when* - the old value only exists at runtime, and the
        // one correct window is after the right-hand side has been evaluated. neither may be a drop
        // pushed *ahead* of the assignment: the right-hand side can read the very value being torn
        // down, so `$a = $a` would become a release before a retain, or a destructor before a copy
        //
        // two fields rather than one, because the two say the same thing differently:
        //
        //  - a **struct** is destroyed in place by an ordinary teardown call in the tree - the same
        //    node emit_drop appends at a scope end, so -ar shows it, the type checker validates it
        //    and a generic teardown instantiates from this call site like any other. it runs
        //    before the store that overwrites those bytes. set for a whole local, a member_path_of
        //    field, and a static property; never for an indexed place, whose target would be
        //    addressed twice
        //  - a **class** owes one reference less, and that cannot be a node. the release needs the old
        //    handle out of the slot codegen already addressed, and a class target may be `$node->next`
        //    or an element whose index expression must not be evaluated twice. so the handle is read
        //    before the store and released after it, which also keeps a deinit from observing the
        //    dying handle through the slot it is leaving
        //
        // null / false when the variable was moved out of - what the slot appears to hold is somebody
        // else's now
        ScopeNode *teardown_old = nullptr;
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
