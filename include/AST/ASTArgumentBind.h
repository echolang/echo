#ifndef ASTARGUMENTBIND_H
#define ASTARGUMENTBIND_H

#pragma once

#include "AST/ExprNode.h"
#include "Token.h"

#include <optional>
#include <string>
#include <vector>

namespace AST
{
    class Collector;
    class FunctionDeclNode;
    class NodeCollection;
    class TypeRegistry;
    struct CodeRef;

    // why a written argument list cannot fill this candidate. bind_arguments reports nothing
    // itself - report_argument_bind_failure words the diagnostic, because a single candidate and
    // an overload set are different sentences
    enum class ArgumentBindKind
    {
        t_ok,
        t_unknown_name,
        t_duplicate_slot,
        t_missing_label,
        t_too_many_positional,
        t_missing_required,
    };

    struct BoundArgument
    {
        // a written argument, or the parameter's default expression still sitting on the decl.
        // apply_argument_binding clones a default; ranking only reads result_type()
        ExprNode *value = nullptr;
        bool from_default = false;
        bool from_caller = false;
    };

    struct ArgumentBinding
    {
        ArgumentBindKind kind = ArgumentBindKind::t_ok;
        std::vector<BoundArgument> filled;
        std::string blame_name;
        std::optional<TokenReference> blame_token;
        size_t missing_param = 0;
    };

    // the filled slots of a successful binding, in parameter order, as ranking and instantiation
    // both need. one owner so CallResolver and can_instantiate cannot drift
    struct BoundSlots
    {
        std::vector<ValueType> types;
        std::vector<ExprNode *> exprs;
        std::vector<bool> defers;
        // a collection dest that cannot fill this literal, whose settled entries cannot sit in
        // that fill, or whose declared core array/map type does not unify with a dest that still
        // names `T`. match_function drops the candidate before argument_fit, which would wait
        // on the still-unknown type
        std::vector<bool> rejected;
    };

    // ranking type of each filled slot. a `from_default` hole scores as the parameter type when
    // that type names no type parameter. an unexpanded collection whose dest can fill it scores
    // as that dest's value type (no `bound_type` write). a dest that cannot fill, or whose settled
    // entries cannot sit in the fill, is `rejected`. a dest that still names `T` scores as
    // collection_literal_type_for (core array/map, the type `$a = [3, 1]` declares) and defers
    // like an untyped scalar, so a typed sibling still decides `T`; a shape that does not unify
    // is `rejected`. pending or empty leaves the slot unknown so a sibling can bind. a pack at
    // `variadic_args` stays unknown so ranking waits on the elements
    BoundSlots bound_slots(
        const FunctionDeclNode &decl,
        const ArgumentBinding &binding,
        Collector &collector);

    // match this argument list against `decl`'s parameters. implicit args (`$this`, a closure
    // environment) occupy the leading slots of `arguments` and are copied through; names start
    // after them. empty `argument_names` means every written argument is positional
    //
    // defaults are not cloned here. a hole that has an `init_expr` is recorded as from_default
    // pointing at that expression, so ranking can see its type and apply_argument_binding is
    // the one that clones
    ArgumentBinding bind_arguments(
        const FunctionDeclNode &decl,
        const std::vector<ExprNode *> &arguments,
        const std::vector<CallArgumentName> &argument_names);

    // rewrite `call.arguments` into parameter order, cloning each default that was used, and
    // drop `argument_names`. the call is then an ordinary positional exact-arity list. asked
    // after a generic callee has been instantiated, so a default mentioning T is cloned from
    // the instance, not the template. a `#[caller]` hole is a varref to the enclosing
    // `$__caller` when that function tracks the caller, otherwise `std::src::here()`. a
    // defaulted `here()` is replanted at this call; an explicit `here()` argument is left
    void apply_argument_binding(
        FunctionCallExprNode &call,
        const ArgumentBinding &binding,
        NodeCollection &nodes,
        Collector &collector);

    // already positional exact-arity: labels were dropped, so bind_arguments must not run
    // again. a cloned/defaulted `here()` is replanted at this call
    void retarget_here_defaults(
        FunctionCallExprNode &call,
        NodeCollection &nodes,
        Collector &collector);

    // word a bind that no candidate survived. a unique name, or every candidate failing the same
    // way (a name nobody has, a missing label, an unfilled required), keeps its own issue; mixed
    // failures in an overload set are NoMatchingOverload
    void report_argument_bind_failure(
        Collector &collector,
        const FunctionCallExprNode &call,
        const std::vector<FunctionDeclNode *> &candidates,
        const CodeRef &at);
};

#endif
