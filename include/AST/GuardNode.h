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
        // cannot live on `decl->init_expr`, because that edge is also the value being tested
        ExprNode *bound_value = nullptr;

        // the `guard` keyword, for the diagnostics that are about the form rather than about its parts
        TokenReference token;

        GuardNode(VarDeclNode *decl, ScopeNode *else_scope, TokenReference token) :
            decl(decl),
            else_scope(else_scope),
            token(token)
        {}

        ~GuardNode() {}

        const std::string node_description() override {
            std::string desc = "guard " + (decl != nullptr ? decl->node_description() : "<none>");

            if (bound_value != nullptr) {
                desc += " bound " + bound_value->node_description();
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
