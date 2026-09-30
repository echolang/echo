#ifndef ASTCASTRESOLUTION_H
#define ASTCASTRESOLUTION_H

#pragma once

#include "AST/ASTFixpointLowering.h"

#include "Token.h"

#include <string>

namespace AST
{
    class Bundle;
    class ExprNode;
    class FunctionDeclNode;
    class TypeCastNode;

    // **classifies a written `$x as T` (or `T(...)`) and rewrites a declared conversion to a call.**
    //
    // a resolution that sometimes lowers: a built-in kind stays a TypeCastNode, an #[implicit]
    // conversion becomes the FunctionCallExprNode AST::emit_declared_conversion mints. leaving a
    // declared conversion as a cast through codegen would grow ownership, type-checking and
    // emission arms for something that is a call.
    //
    // **inside AST::Monomorphizer's fixpoint**, after MatchResolution and before InterpolationLowering
    // / OwnershipPass. a match arm can yield the value being cast; a declared conversion is a call
    // settle_calls and the ownership walk have to see. OwnershipPass::body_is_concrete answers
    // false while an explicit TypeCastNode is still undecided, so the walk waits.
    //
    // **reports and keeps** on a refusal, AST::GuardLowering's rule: the operand may still be read
    // after this node.
    //
    // **walks t_owned bodies** via AST::function_needs_typing. a user `as` can settle after
    // ownership marks t_owned; a teardown const-strip is classified at mint
    class CastResolution : private FixpointLowering
    {
    public:
        CastResolution(Bundle &bundle);

        using FixpointLowering::run_round;
        using FixpointLowering::finalize;

    private:
        void visitFunctionDecl(FunctionDeclNode &node) override;

        ExprNode *rewrite_value_edge(ExprNode *expr) override;

        ExprNode *resolve(TypeCastNode &node);

        void refuse(TypeCastNode &node, const TokenReference &at, std::string why);
    };
};

#endif
