#ifndef GUARDNODE_H
#define GUARDNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ScopeNode.h"
#include "AST/ExprNode.h"
#include "AST/VarDeclNode.h"

namespace AST
{
    // `T $x = guard <v> else { ... }` binds; `guard <v> else { ... }` does not.
    // the declaration is an ordinary local of the enclosing scope. the else arm must leave
    // (scope_always_exits): falling through would leave `$x` bound to a value that is not there
    class GuardNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_guard);

        // the binding, already typed non-null. its `init_expr` is the nullable being tested, and it is
        // the *same* expression - evaluated once, on the path that enters.
        //
        // **null is the statement form** - `guard <expr> else { ... }` - success with nothing to bind.
        // the subject then lives on `subject` below. AST::GuardNode::tested() is the one reader of
        // whichever storage holds it
        VarDeclNode *decl = nullptr;

        // **the tested expression, when there is no binding.** null on the initializer form, where
        // the subject *is* `decl->init_expr` and must stay that (the T? path evaluates it once for
        // both the test and the unwrap). a second copy here would double-evaluate
        ExprNode *subject = nullptr;

        // where control goes when the value is absent
        ScopeNode *else_scope = nullptr;

        // written by OwnershipPass only. a lowering that seats the payload here skips the copy
        // constructor and double-frees an owning one. protocol form writes `decl->init_expr` instead
        ExprNode *bound_value = nullptr;

        // which form the statement is. null: a `T?`, tested by gen_has_value. set: the protocol
        // `has_value()` call, branched on before `decl->init_expr` unwraps inside the bound block
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

        // **the one reader of "what is being tested."** initializer form stores it on the binding
        // (`decl->init_expr` is also the T? unwrap); statement form stores it on `subject`. do not
        // re-derive the ternary at each site
        ExprNode *tested() const {
            return decl != nullptr ? decl->init_expr : subject;
        }

        // **the one writer of the same.** OperatorRewriter's weak upgrade, the parser seating the
        // subject - anything that mutates "what is being tested". a getter with no setter is how
        // the statement form missed the generic `weak<T>` upgrade
        void set_tested(ExprNode *expr) {
            if (decl != nullptr) {
                decl->init_expr = expr;
            } else {
                subject = expr;
            }
        }

        // **the existing text is byte for byte what it was**, and the new clauses append only when the
        // new edges are non-null - which is what keeps four RAST goldens intact across a syntax change
        // and a protocol. a `T?` guard renders exactly as it always did. the statement form names the
        // expression rather than a binding
        const std::string node_description() override {
            std::string desc = "guard ";

            if (decl != nullptr) {
                desc += decl->node_description();
            } else if (subject != nullptr) {
                desc += subject->node_description();
            } else {
                desc += "<none>";
            }

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
