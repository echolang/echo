#ifndef ASTEMBED_H
#define ASTEMBED_H

#pragma once

#include "AST/ASTBuiltin.h"

#include <string>

namespace AST
{
    class Collector;
    class FunctionCallExprNode;
    struct EmbeddedResource;

    enum class EmbedKind
    {
        t_text,
        t_bytes,
    };

    enum class EmbedStatus
    {
        t_ready,
        t_pending,
        t_refused,
    };

    struct EmbedPlan
    {
        EmbedStatus status = EmbedStatus::t_pending;
        EmbedKind kind = EmbedKind::t_text;
        const EmbeddedResource *resource = nullptr;
        std::string refusal;
    };

    // is this one of the two embed builtins. a spelling so TypeChecker and codegen do not
    // each hardcode the pair
    bool is_embed_builtin(BuiltinKind kind);

    EmbedKind embed_kind_of(BuiltinKind kind);

    // **the sole answer to "what does this embed call put in the binary".** three answers, for
    // unwrap_plan_for's reason: a call whose argument is not a literal yet must not read as a
    // refusal, and codegen never sees a pending one.
    //
    // interned on the path token's file's module, so a cloned call still finds the bytes.
    // `collector` spells the UTF-8 refusal; codegen omits it. the sidecar list is TypeChecker's:
    // it records `plan.resource->resolved` on `_current_module` when the plan is ready
    EmbedPlan embed_plan_for(FunctionCallExprNode &call, const Collector *collector = nullptr);
};

#endif
