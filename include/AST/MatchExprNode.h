#ifndef MATCHEXPRNODE_H
#define MATCHEXPRNODE_H

#pragma once

#include "AST/ASTNode.h"
#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"
#include "Token.h"

#include <optional>
#include <vector>

namespace AST
{
    class ScopeNode;
    class VarDeclNode;

    // **the one way to read which case an enum is holding.**
    //
    //     $meters = match ($unit) {
    //         Unit::meter($v)     => $v,
    //         Unit::kilometer($v) => $v * 1000,
    //         else                => 0,
    //     };
    //
    // an **expression**, and a statement only in the sense that any expression may be one. that is not
    // symmetry for its own sake: an arm produces a value, and there is nowhere else in Echo for a value
    // produced by a branch to come from - `??` is the only other one and it has exactly two arms.
    //
    // **a node that reaches codegen, rather than a lowering inside the fixpoint.** the three passes that
    // rewrite in there - AST::ConstFolding, AST::ForeachLowering, AST::InterpolationLowering - each turn
    // a statement into statements, and notes/control_flow.md's rule is what separates this from them:
    // `for` is a node because the difference it makes is a basic block, `foreach` is not because it
    // makes none. a match makes N of them and a phi, and lowering it would mean hoisting a declaration
    // out of an arbitrary expression position, which changes the order the operands around it evaluate
    // in. so it is emitted, by ExprCodegen::gen_match, as a switch over `__tag` and a phi
    //
    // **a pattern names a case, and the parenthesised names after it bind that case's payload.** they
    // take no part in choosing the arm - `Unit::meter` and `Unit::meter($v)` select identically, and the
    // second additionally says what to call what is inside. so there is no arm ordering to reason about
    // and no overlap between two patterns naming one case: that is a duplicate, and it is refused
    class MatchExprNode : public ExprNode
    {
    public:
        ECO_AST_NODE_TYPE(n_expr_match);

        // one arm. the pattern is a *name* until the fixpoint settles the subject's type, because
        // `.timeout($s)` says which enum it means only by where it is
        struct Arm
        {
            // the pattern's own token - the case name, or the `else`. every diagnostic about this arm
            // points here
            TokenReference token;

            // the case this arm selects, once patterns_decided. nullopt on the `else` arm - but never
            // read as the test for one: it is also nullopt on every arm of a match the fixpoint has not
            // reached yet, so "is this the catch-all" and "which case did resolution pick" are two
            // questions settled at two moments. is_else() is the first of them
            std::optional<size_t> case_ordinal;

            // the case name as written, which is what resolution matches and what a "no such case"
            // diagnostic quotes. empty on the `else` arm
            std::string case_name;

            // the owner the pattern wrote, or null for `.name` and a bare `name` - which take the
            // subject's own type. kept rather than resolved at parse time because the two have to
            // *agree*: an owner naming a different enum than the subject is a diagnostic, and it is one
            // resolution can only give once the subject has settled
            TypeNode *owner = nullptr;

            // the arm's own scope. its first children are the payload bindings, seeded there by the
            // parser exactly as a `foreach`'s bindings and a `guard`'s failure are - so they are
            // ordinary locals of an ordinary block, the frame machinery ends them, and no ownership
            // rule, drop rule or codegen arm appears anywhere for a binding
            ScopeNode *scope = nullptr;

            // the value this arm produces, or null for a `{ ... }` arm - which is what makes the whole
            // match `void`. one edge rather than two arm kinds, because "this arm produces nothing" and
            // "this match produces nothing" have to be the same answer or an arm list could disagree
            // with itself
            ExprNode *value = nullptr;

            // **the catch-all, and the name alone answers it.** an `else` is the one arm the parser
            // pushes without a case name, and it is the only thing that ever leaves one empty - so this
            // is true from the moment the arm exists rather than from the round that resolved it
            bool is_else() const {
                return case_name.empty();
            }
        };

        // the scrutinee, as a declaration this node owns - AST::TemporaryBindExprNode's shape and for
        // its reason: the arms read the payload off it, so it has to have storage, and the storage has
        // to belong to something that ends it. `init_expr` is the expression as written, evaluated
        // exactly once on entry
        VarDeclNode *subject = nullptr;

        std::vector<Arm> arms;

        // **has the fixpoint answered which cases these patterns name?** false is what makes
        // OwnershipPass::body_is_concrete answer false for the body holding it, and that arm is
        // load-bearing for the reason GuardNode::plan_decided states: the pass walks a body exactly
        // once, ever, so a walk taken before the bindings were typed would resolve the arrival of a
        // value about to be replaced - permanently, and with nothing reporting it
        bool patterns_decided = false;

        // the unified type of the arms, settled beside patterns_decided. `void` for a match every arm
        // of which is a block, which is the statement form
        ValueType result = ValueType::make_unknown();

        // **does this match hand back storage rather than a value?** true when every arm that produces
        // anything produces a *place*, which for an enum is the ordinary shape - a payload binding is a
        // borrow into the subject, so `E::one($v) => $v` names storage the match did not invent.
        //
        // a field rather than an answer derived from `result`, because the two say different things: a
        // match whose arms are `ptr<T>` *values* also has a pointer `result` and is not a place. read
        // through AST::read_reaches_storage, which is what every question about the *form* asks, and
        // per arm through arm_yields_address below
        //
        // it is what lets an enum answer `contract::unwrappable<V>::unwrap() : V&`: the `ok` arm hands the
        // payload's storage out, and the `error` arm - which has no `V` at all - stops the program
        bool yields_a_place = false;

        // **does this arm hand the phi an address?** the place edge AST::RecursiveVisitor walks and the
        // address ExprCodegen's phi is built from are one question asked in two places, and they have to
        // agree: a value edge inserts the auto-deref, so an arm walked as one and then addressed by the
        // phi reads the payload rather than pointing at it. an arm that never comes back contributes
        // nothing to either, for the reason it was exempt from the unification too
        bool arm_yields_address(const Arm &arm) const;

        // the `match` keyword, for the diagnostics that are about the form rather than about an arm
        TokenReference token;

        MatchExprNode(VarDeclNode *subject, const TokenReference &token) :
            subject(subject), token(token)
        {};

        ~MatchExprNode() {};

        ValueType result_type() const override {
            return result;
        }

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_match(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
