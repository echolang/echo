#include "AST/ASTInstantiation.h"

#include "AST/ASTTypeParam.h"
#include "AST/ASTTypeUnify.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"

#include <algorithm>

namespace AST
{
    namespace
    {
        Instantiation rejected(InstantiationBlame blame)
        {
            Instantiation result;
            result.fit = InstantiationFit::t_no;
            result.blame = blame;
            return result;
        }
    }

    std::optional<size_t> first_constraint_violation(
        const std::vector<TypeParamDecl *> &params,
        const std::vector<ValueType> &args)
    {
        const size_t count = std::min(params.size(), args.size());

        for (size_t i = 0; i < count; i++) {
            // a bare type parameter is the one argument whose answer genuinely is not knowable yet:
            // it stands for whatever the enclosing template will be instantiated with, and is judged
            // then. `Box<T>` is deliberately not that case - it will be a Box whatever T becomes, so
            // its answer is already decided
            //
            // unknown and void carry no information at all, and a program that got one of those here
            // has already been told why - a constraint error on top would blame one typo twice
            if (args[i].is_type_param() || args[i].is_unknown() || args[i].is_void()) {
                continue;
            }

            if (!params[i]->allows(args[i])) {
                return i;
            }
        }

        return std::nullopt;
    }

    Instantiation can_instantiate(
        const FunctionDeclNode *tmpl,
        const std::vector<ValueType> &argument_types,
        const std::vector<ValueType> &explicit_type_args)
    {
        Instantiation result;

        // which argument's shape could not be reconciled. collected rather than returned on, so
        // that the parameters can still be judged - a template with one bad argument is usually
        // still a template naming a perfectly good instance, and the type checker is the one that
        // reports the argument
        std::optional<size_t> mismatched_argument;

        // did an argument with no type yet decline to bind anything? it is the difference between
        // the two unresolved blames: a parameter nothing *can* bind is a "cannot infer", while one
        // that an untyped argument would have bound is a not-yet, and the fixpoint answers it
        bool saw_untyped_argument = false;

        if (!explicit_type_args.empty()) {
            // explicit type arguments win: foo<int>(...)
            //
            // a method carries its owner's parameters ahead of its own, and only the *own* ones can
            // be spelled at the call site: `$b->map<float64>()` says nothing about Box's T, which
            // the receiver already fixes
            if (explicit_type_args.size() != tmpl->own_type_param_count()) {
                return rejected(InstantiationBlame::t_type_argument_count);
            }

            const size_t inherited = tmpl->inherited_type_param_count;

            // the owner's parameters come from the receiver, which is argument 0. inferred rather
            // than read off the receiver's instantiation_args so there is one binding rule: the
            // receiver parameter is `Box<T>&` and unify_type already descends a generic application
            if (inherited > 0 && !argument_types.empty() && !tmpl->args.empty() && tmpl->args[0]->has_type()) {
                unify_type(tmpl->args[0]->type(), argument_types[0], result.bindings);
            }

            for (size_t i = 0; i < explicit_type_args.size(); i++) {
                result.bindings.bind(tmpl->type_parameters[inherited + i], explicit_type_args[i]);
            }

            // deliberately no argument-count check on this branch: the type arguments are what the
            // instance is named after, and a call passing the wrong number of *arguments* to a
            // template it named correctly is an ordinary bad call, reported against the instance
        } else {
            if (tmpl->args.size() != argument_types.size()) {
                return rejected(InstantiationBlame::t_argument_count);
            }

            for (size_t i = 0; i < argument_types.size(); i++) {
                // an argument with no type yet cannot contradict the template, and must not bind
                // anything either: binding `T` to unknown would name the instance after
                // information that has not arrived. it is the reason for the t_maybe answer below
                if (is_undetermined_type(argument_types[i])) {
                    saw_untyped_argument = true;
                    continue;
                }

                // a parameter with no type at all is a hole left by a failed parse, already
                // reported where it was read. not a not-yet - nothing is coming for it
                if (!tmpl->args[i]->has_type()) {
                    continue;
                }

                if (!unify_type(tmpl->args[i]->type(), argument_types[i], result.bindings)
                    && !mismatched_argument.has_value()) {
                    mismatched_argument = i;
                }
            }
        }

        // unification binds in *argument* order; a type-argument list has to come back out in
        // *declaration* order, because that order is what identifies the instantiation
        std::vector<ValueType> bound_types;
        bound_types.reserve(tmpl->type_parameters.size());

        const TypeParamDecl *unbound = nullptr;
        const TypeParamDecl *undecided = nullptr;
        std::optional<size_t> unresolved_index;

        for (size_t i = 0; i < tmpl->type_parameters.size(); i++) {
            const TypeParamDecl *param = tmpl->type_parameters[i];
            const ValueType *bound = result.bindings.lookup(param);
            const bool resolved = bound != nullptr && !is_undetermined_type(*bound);

            bound_types.push_back(resolved ? *bound : ValueType::make_unknown());

            if (resolved) {
                continue;
            }

            if (!unresolved_index.has_value()) {
                unresolved_index = i;
            }

            // a parameter nothing *can* bind is a "cannot infer". one that an untyped argument would
            // have bound, or that is bound to something still mentioning a type parameter, is a
            // not-yet. an *owner's* parameter is always the second kind: it comes from the receiver,
            // and no call site can name it explicitly, so there is nothing to tell the user to write
            const bool retryable = bound != nullptr
                || saw_untyped_argument
                || i < tmpl->inherited_type_param_count;

            if (retryable) {
                if (undecided == nullptr) {
                    undecided = param;
                }
            } else if (unbound == nullptr) {
                unbound = param;
            }
        }

        const std::optional<size_t> violation = first_constraint_violation(tmpl->type_parameters, bound_types);

        // the fit, which is all overload resolution reads. an argument whose shape cannot be
        // reconciled is a definite no - no substitution will ever make it fit - while a parameter
        // nobody has decided yet is only a not-yet. between the two parameter-level complaints the
        // first one in declaration order decides, so a constraint the user can see violated is not
        // hidden behind a later parameter that has not been inferred
        //
        // a violation with nothing unresolved is always `constraint_first`, so the two no-arms are
        // one: t_maybe is reached only by a violation that a *later* parameter's constraint reports
        const bool constraint_first = violation.has_value()
            && (!unresolved_index.has_value() || *violation < *unresolved_index);

        if (mismatched_argument.has_value() || constraint_first) {
            result.fit = InstantiationFit::t_no;
        } else if (unresolved_index.has_value()) {
            result.fit = InstantiationFit::t_maybe;
        } else {
            result.fit = InstantiationFit::t_yes;
        }

        // the blame, in reporting order, which is not the fit order: everything a diagnostic can
        // name outranks a shape mismatch, because that one is the type checker's to report
        if (unbound != nullptr) {
            result.blame = InstantiationBlame::t_unbound_parameter;
            result.param = unbound;
        } else if (undecided != nullptr) {
            result.blame = InstantiationBlame::t_undecided_parameter;
            result.param = undecided;
        } else if (violation.has_value()) {
            result.blame = InstantiationBlame::t_constraint;
            result.param = tmpl->type_parameters[*violation];
            result.bound = bound_types[*violation];
        } else if (mismatched_argument.has_value()) {
            result.blame = InstantiationBlame::t_argument_shape;
            result.argument = *mismatched_argument;
        }

        result.decided = !unresolved_index.has_value();

        if (result.decided) {
            result.type_arguments = std::move(bound_types);
        }

        return result;
    }

    std::vector<ValueType> argument_types_of(const FunctionCallExprNode &call)
    {
        std::vector<ValueType> argument_types;
        argument_types.reserve(call.arguments.size());

        for (const auto *arg : call.arguments) {
            argument_types.push_back(arg ? arg->result_type() : ValueType::make_unknown());
        }

        return argument_types;
    }

    std::vector<ValueType> explicit_type_args_of(const FunctionCallExprNode &call)
    {
        std::vector<ValueType> explicit_type_args;
        explicit_type_args.reserve(call.explicit_type_args.size());

        for (const auto *type_node : call.explicit_type_args) {
            explicit_type_args.push_back(type_node ? type_node->type : ValueType::make_unknown());
        }

        return explicit_type_args;
    }

    Instantiation can_instantiate(const FunctionDeclNode *tmpl, const FunctionCallExprNode &call)
    {
        return can_instantiate(tmpl, argument_types_of(call), explicit_type_args_of(call));
    }
};
