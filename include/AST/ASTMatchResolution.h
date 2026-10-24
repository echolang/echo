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

    // which case each arm of a `match` selects, and what its bindings hold. a resolution, not a
    // lowering: the node stays for codegen. every diagnostic about a match's shape is here.
    // inside the fixpoint, before ownership. body_is_concrete is false until patterns_decided
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
