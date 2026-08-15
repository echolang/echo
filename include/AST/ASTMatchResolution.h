#ifndef ASTMATCHRESOLUTION_H
#define ASTMATCHRESOLUTION_H

#pragma once

#include "AST/ASTFixpointLowering.h"

#include "Token.h"

#include <string>

namespace AST
{
    class Bundle;
    class FunctionDeclNode;
    class MatchExprNode;

    // **which case each arm of a `match` selects, and what each of its bindings holds.**
    //
    // a resolution rather than a lowering, which is the whole difference from AST::GuardLowering and
    // AST::ForeachLowering beside it: those two replace a node with statements, and this one fills a
    // node in and leaves it for codegen. what it has in common with them is the *moment* - the subject's
    // type is exactly what no parse could have answered, `.timeout($s)` naming its enum only by where it
    // sits, so this cannot run before the fixpoint has settled the subject.
    //
    // **inside the monomorphizer's fixpoint**, after the stale-variable sweep that types the subject
    // and before the ownership pass, which walks a body exactly once ever - safe rather than merely
    // early, because `body_is_concrete` answers false while `MatchExprNode::patterns_decided` is false.
    //
    // **every diagnostic about a match's shape is here**, and deliberately not spread between here and
    // AST::TypeChecker: what a pattern names, how many bindings a case has and whether the arms cover
    // the enum are one question asked of one table, and the checker would have to re-derive that table
    // to ask any of them. what is left for the checker is what it already does for every other tree -
    // the bindings are ordinary declarations with ordinary initializers by then, and an arm's value is
    // an ordinary expression
    //
    // **the walk, the exit obligation and the generic-body skip are AST::FixpointLowering's.** this
    // pass hoists nothing, so it never asks next_hoist_index(); what it needed from the chassis is
    // exactly the three invariants that used to be a fourth copy here
    class MatchResolution : private FixpointLowering
    {
    public:
        MatchResolution(Bundle &bundle);

        using FixpointLowering::run_round;
        using FixpointLowering::finalize;

    private:
        void visitFunctionDecl(FunctionDeclNode &node) override;

        // the node itself, reached as an ordinary expression edge - a match may sit anywhere a value
        // may, so there is no statement position to walk instead
        void visit_match(MatchExprNode &node) override;

        // fills the node in, or leaves it alone for another round. **reports and keeps** on a refusal,
        // AST::GuardLowering's rule: the arms are read after this node, so forgetting the subtree would
        // free declarations those reads still point at - the unsound direction of NodeCollection::forget
        void resolve(MatchExprNode &node);

        // marks the node answered and its arms harmless, so nothing asks again and codegen is never
        // reached with a half-filled one - has_critical_issues() stops the build first
        void refuse(MatchExprNode &node, const TokenReference &at, std::string why);

        // the function whose body is being walked, so a `return match { => &add }` can bind
        // an overloaded `&name` to the declared return type. null at file scope
        FunctionDeclNode *_enclosing_function = nullptr;
    };
};

#endif
