#ifndef ASTCONSTRUCTFORWARD_H
#define ASTCONSTRUCTFORWARD_H

#pragma once

#include "AST/ASTArgumentBind.h"
#include "AST/ASTValueType.h"

#include <optional>
#include <string>
#include <vector>

namespace AST
{
    class FunctionCallExprNode;
    class FunctionDeclNode;
    class FunctionRegistry;
    class Module;
    class NodeCollection;
    class TypeRegistry;
    class VarDeclNode;
    struct CallArgumentName;

    // **which parameter gathers constructor arguments?** the last user parameter when it was
    // written `construct M $value`, null otherwise. CallResolver is the only asker of the wrap;
    // TypeChecker asks `construct_refusal` of the declaration
    const VarDeclNode *construct_param_of(const FunctionDeclNode &decl);
    VarDeclNode *construct_param_of(FunctionDeclNode &decl);

    // **does this call gather constructor arguments for this candidate?** the first written
    // argument that builds M, counting the implicit prefix. nullopt when there is no construct
    // param or the written list is not longer than the prefix: ordinary bind, including the
    // already-exact-M case CallResolver still scores as a value of M
    std::optional<size_t> construct_gather_start(const FunctionDeclNode &decl, size_t argument_count);

    // constructors of `type`, the same set a written `M(...)` uses. empty for a primitive, a
    // type parameter, or a type with no name in a namespace
    const std::vector<FunctionDeclNode *> &constructors_of(
        const ValueType &type,
        const FunctionRegistry &functions
    );

    // **should this candidate gather, and which slice is the constructor args?** ranking asks
    // this and scores a virtual last argument of type M; CallResolver mints `M(...)` only after
    // a winner is chosen
    struct ConstructWrap
    {
        enum class Kind
        {
            t_none,
            t_wrap,
            t_pending,
            t_unconstructable,
        };

        Kind kind = Kind::t_none;
        ArgumentBinding binding;
        ValueType target;
        size_t remaining_start = 0;
        std::vector<FunctionDeclNode *> constructors;
    };

    ConstructWrap consider_construct_wrap(
        const FunctionDeclNode &candidate,
        const FunctionCallExprNode &call,
        TypeRegistry &registry,
        const FunctionRegistry &functions,
        const Module *module
    );

    // **mint M(...) into the construct slot.** ranking already scored a virtual last argument of
    // type M; CallResolver asks this once a winner is chosen
    void apply_construct_wrap(
        FunctionCallExprNode &call,
        const ConstructWrap &plan,
        NodeCollection &nodes
    );

    // **why may `construct` not sit where it was written?** nullopt when it may.
    //
    // asked once of every declaration by AST::TypeChecker, like variadic_args_refusal. a type
    // parameter is silence: which it becomes is not knowable until the call binds it
    std::optional<std::string> construct_refusal(const FunctionDeclNode &decl);
};

#endif
