#ifndef GUARDNODE_H
#define GUARDNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ScopeNode.h"
#include "AST/ExprNode.h"
#include "AST/VarDeclNode.h"

namespace AST
{
    // `guard T $x = <nullable> else { ... }` - bind the value if it is there, and leave otherwise
    //
    // one of the three forms that read through a `T?`, and the only one that is a *statement*. it is what
    // the other two are compared against: `??` supplies a replacement and `?->` skips the work, while this
    // one says the rest of the scope has no meaning without the value - which is usually the truth
    //
    // **a nullability feature, not a weak one.** the initializer may be any `T?` whatever `T` is - a
    // nullable primitive, struct, class, interface or callable, or a `ptr<T>`, which is the same flag on a
    // pointer level. a `weak<T>` is *also* accepted, by being upgraded first, and that acceptance is
    // decided in one place for all three forms (AST::optional_operand_of)
    //
    // the declaration lands in the **enclosing** scope, holding the non-null type. so it is an ordinary
    // local from that point on: the existing frame machinery drops it at scope end, and no new lifetime
    // rule appears anywhere. that is the whole reason this is a statement and not an expression
    //
    // the else block **must not fall through** - it has to return, break, continue or stop the program. a
    // guard whose else arm ran on and rejoined would leave `$x` bound to a value that is not there, which
    // is the one thing this form exists to prevent. AST::scope_always_exits is the rule, checked at the
    // declaration rather than left to codegen
    class GuardNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_guard);

        // the binding, already typed non-null. its `init_expr` is the nullable being tested, and it is
        // the *same* expression - evaluated once, on the path that enters
        VarDeclNode *decl = nullptr;

        // where control goes when the value is absent
        ScopeNode *else_scope = nullptr;

        // **what the binding is given on the path that enters**, when unwrapping the tested value is not
        // the whole of it.
        //
        // null is the common case and the whole no-regression story: a payload that copies as bytes, a
        // class handle, a weak - every nullable in the tree before tagged optionals owned anything - leaves
        // this unset and codegen stores the unwrapped value exactly as it always did.
        //
        // it is set for one shape: a tagged optional whose payload needs a real copy, read out of a
        // *place*. There the binding and the optional are two owners of two values, so the payload has to
        // be copied rather than moved out from under a value somebody else still holds - and the copy
        // cannot live on `decl->init_expr`, because on that form that edge is also the value being tested.
        //
        // **written by AST::OwnershipPass and by nothing else.** a lowering that handed the binding
        // its value on an edge of its own would skip the copy constructor: the payload would be
        // byte-copied out of storage somebody else still owns, and both ends would then be
        // destroyed. the protocol form puts its `deref(unwrap())` on `decl->init_expr` instead,
        // where the ordinary arrival machinery covers it with no arm at all
        ExprNode *bound_value = nullptr;

        // **the presence question, when the type answers it rather than the machine.**
        //
        // null is the common case and the whole no-regression story: a `T?` is tested with one
        // extractvalue or one null compare, which is TypeLowering::gen_has_value, and nothing about that
        // path moves.
        //
        // non-null for a subject declaring `contract::unwrappable<V>`: it is the `has_value()` call
        // AST::GuardLowering minted, and it is the value codegen branches on. there is no optional left
        // to evaluate there, the subject having been hoisted into an ordinary declaration ahead of this
        // statement - so `decl->init_expr` holds the protocol's `deref(unwrap())` and is an ordinary
        // declaration initializer in every sense the rest of the compiler cares about.
        //
        // **this pointer is therefore which form the statement is**, and three places read it as that:
        // AST::OwnershipPass keys its arm on it, AST::TypeChecker asks "is this certainly present"
        // only for the other form, and StmtCodegen::gen_guard branches on it twice.
        //
        // the invariant, stated once: **`presence_test` if set is the value evaluated before the branch
        // and `decl->init_expr` is then evaluated inside the bound block; with none, `init_expr` is
        // evaluated before the branch and is both what is tested and what is unwrapped. either way
        // nothing is evaluated twice and nothing on the absent path is evaluated at all**
        ExprNode *presence_test = nullptr;

        // `else ($e)` - the reason the subject was not holding a value, when it declares
        // `contract::failable<E>`.
        //
        // **not an owned edge.** the declaration is `else_scope->children[0]`, seeded there by the
        // parser exactly as a `foreach`'s bindings are, so it is the else arm's own local and the
        // ordinary frame machinery ends it - no ownership rule, no codegen and no drop rule. this
        // pointer is the cross-reference AST::GuardLowering fills the initializer of, which is why
        // `clone` rebinds it rather than cloning it
        VarDeclNode *failure = nullptr;

        // **has the fixpoint answered how this is unwrapped?** true straight from the parser for a `T?`,
        // whose payload is a property of the type and needs nobody's conformance.
        //
        // false is what makes `OwnershipPass::body_is_concrete` answer false for the body holding it, and
        // that arm is load-bearing: this pass walks a body exactly once, ever, so a walk taken before
        // the plan landed would resolve the arrival of a value about to be replaced by an `unwrap()`
        // read - permanently, and with nothing reporting it. the same shape
        // `IndexExprNode::resolution_decided` and `ArrayLiteralExprNode::expansion_decided` already own
        bool plan_decided = false;

        // the author omitted `else`. the else_scope still holds a never-returning abort, so
        // AST::scope_always_exits stays the sole owner of "does this leave?". this bit is what
        // a dump reads, so the tree does not have to be expanded just to say so
        bool implicit_abort = false;

        // the `guard` keyword, for the diagnostics that are about the form rather than about its parts
        TokenReference token;

        GuardNode(VarDeclNode *decl, ScopeNode *else_scope, TokenReference token) :
            decl(decl),
            else_scope(else_scope),
            token(token)
        {}

        ~GuardNode() {}

        // **the existing text is byte for byte what it was**, and the new clauses append only when the
        // new edges are non-null - which is what keeps four RAST goldens intact across a syntax change
        // and a protocol. a `T?` guard renders exactly as it always did
        const std::string node_description() override {
            std::string desc = "guard " + (decl != nullptr ? decl->node_description() : "<none>");

            if (presence_test != nullptr) {
                desc += " if " + presence_test->node_description();
            }

            if (bound_value != nullptr) {
                desc += " bound " + bound_value->node_description();
            }

            if (failure != nullptr) {
                desc += " failure " + failure->node_description();
            }

            if (implicit_abort) {
                desc += " implicit-abort";
                return desc;
            }

            desc += " else\n";
            desc += else_scope != nullptr ? else_scope->node_description() : "{}";
            return desc;
        }

        void accept(Visitor &visitor) override {
            visitor.visit_guard(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
