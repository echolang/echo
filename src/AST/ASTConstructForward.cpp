#include "AST/ASTConstructForward.h"

#include "AST/ASTAccess.h"
#include "AST/ASTArgumentFit.h"
#include "AST/ASTCollectionLiteral.h"
#include "AST/ASTConstructor.h"
#include "AST/ASTFunctionRegistry.h"
#include "AST/ASTInstantiation.h"
#include "AST/ASTLiteralTyping.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTModule.h"
#include "AST/ASTNode.h"
#include "AST/ASTNullability.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/VarDeclNode.h"

#include <algorithm>
#include <cstddef>
#include <fmt/core.h>

namespace AST
{
    namespace
    {
        std::optional<size_t> find_construct_index(const FunctionDeclNode &decl)
        {
            const size_t implicit = decl.implicit_arg_count();
            if (decl.args.size() <= implicit) {
                return std::nullopt;
            }

            const size_t last = decl.args.size() - 1;
            const VarDeclNode *param = decl.args[last];
            if (param == nullptr || !param->constructs_in_place) {
                return std::nullopt;
            }

            return last;
        }

        std::vector<CallArgumentName> construct_inner_argument_names(
            const std::vector<CallArgumentName> &argument_names,
            size_t remaining_start,
            size_t argument_count,
            const VarDeclNode *param
        )
        {
            std::vector<CallArgumentName> inner_names;
            if (argument_count <= remaining_start) {
                return inner_names;
            }

            inner_names.reserve(argument_count - remaining_start);
            const CallArgumentName k_positional {};

            for (size_t i = remaining_start; i < argument_count; i++) {
                const CallArgumentName &written =
                    i < argument_names.size() ? argument_names[i] : k_positional;
                const bool strip_label = i == remaining_start
                    && written.kind == ArgumentNameKind::t_label
                    && param != nullptr
                    && param->has_label()
                    && written.token.has_value()
                    && written.token->value() == param->label();
                if (strip_label) {
                    inner_names.emplace_back();
                } else {
                    inner_names.push_back(written);
                }
            }

            return inner_names;
        }

        std::optional<ArgumentBinding> bind_construct_prefix(
            const FunctionDeclNode &decl,
            const std::vector<ExprNode *> &arguments,
            const std::vector<CallArgumentName> &argument_names,
            size_t remaining_start
        )
        {
            if (remaining_start > arguments.size()) {
                return std::nullopt;
            }

            const std::vector<ExprNode *> prefix(
                arguments.begin(),
                arguments.begin() + static_cast<std::ptrdiff_t>(remaining_start)
            );
            std::vector<CallArgumentName> prefix_names;
            if (!argument_names.empty() && remaining_start > 0) {
                const size_t n = std::min(argument_names.size(), remaining_start);
                prefix_names.reserve(n);
                for (size_t i = 0; i < n; i++) {
                    prefix_names.push_back(argument_names[i]);
                }
            }

            ArgumentBinding binding = bind_arguments(decl, prefix, prefix_names);
            const bool construct_hole = binding.kind == ArgumentBindKind::t_missing_required
                && binding.missing_param == remaining_start;
            if (binding.kind != ArgumentBindKind::t_ok && !construct_hole) {
                return std::nullopt;
            }

            binding.kind = ArgumentBindKind::t_ok;
            return binding;
        }

        // dest-dependent forms: wrapping is what names them. everything else that is still
        // untyped may type as M on its own, and wrapping would nest `M(M(...))`
        bool types_only_at_destination(ExprNode *only)
        {
            only = peel_move(only);
            if (only == nullptr) {
                return false;
            }

            return unbound_shorthand_call_of(only) != nullptr
                || collection_literal_of(only) != nullptr
                || is_untyped_literal(only)
                || is_written_null(only);
        }
    }

    const VarDeclNode *construct_param_of(const FunctionDeclNode &decl)
    {
        const auto index = find_construct_index(decl);
        return index.has_value() ? decl.args[*index] : nullptr;
    }

    VarDeclNode *construct_param_of(FunctionDeclNode &decl)
    {
        const auto index = find_construct_index(decl);
        return index.has_value() ? decl.args[*index] : nullptr;
    }

    std::optional<size_t> construct_gather_start(const FunctionDeclNode &decl, size_t argument_count)
    {
        const auto index = find_construct_index(decl);
        if (!index.has_value() || argument_count <= *index) {
            return std::nullopt;
        }

        return index;
    }

    const std::vector<FunctionDeclNode *> &constructors_of(
        const ValueType &type,
        const FunctionRegistry &functions
    )
    {
        static const std::vector<FunctionDeclNode *> empty;

        if (is_undetermined_type(type) || !type.has_complex_type()) {
            return empty;
        }

        const ComplexType *owner = type.get_complex_type()->template_or_self();
        if (owner == nullptr || !owner->name.has_value() || owner->ast_namespace == nullptr) {
            return empty;
        }

        return functions.overloads(*owner->name, *owner->ast_namespace);
    }

    ConstructWrap consider_construct_wrap(
        const FunctionDeclNode &candidate,
        const FunctionCallExprNode &call,
        TypeRegistry &registry,
        const FunctionRegistry &functions,
        const Module *module
    )
    {
        ConstructWrap result;
        const auto remaining_at = construct_gather_start(candidate, call.arguments.size());
        if (!remaining_at.has_value()) {
            return result;
        }

        const size_t remaining_start = *remaining_at;
        const VarDeclNode *param = candidate.args[remaining_start];
        if (param == nullptr) {
            return result;
        }

        ValueType target = param->type();
        InstantiationFit target_fit = InstantiationFit::t_yes;

        if (candidate.is_generic() || contains_type_param(target)) {
            std::vector<ValueType> prefix_types;
            std::vector<bool> prefix_defers;
            prefix_types.reserve(remaining_start);
            prefix_defers.reserve(remaining_start);
            for (size_t i = 0; i < remaining_start; i++) {
                ExprNode *arg = call.arguments[i];
                prefix_types.push_back(
                    arg != nullptr ? arg->result_type() : ValueType::make_unknown());
                prefix_defers.push_back(is_untyped_literal(arg));
            }

            const ValueType &owner = call.static_owner.is_unknown()
                ? call.constructed_type : call.static_owner;
            const Instantiation inst = can_instantiate(
                &candidate,
                prefix_types,
                registry,
                explicit_type_args_of(call),
                owner,
                prefix_defers);

            target_fit = inst.fit;
            if (inst.decided || inst.fit == InstantiationFit::t_yes) {
                target = substitute_type(param->type(), inst.bindings, registry);
            }
        }

        if (is_undetermined_type(target) || contains_type_param(target)) {
            if (target_fit == InstantiationFit::t_maybe) {
                result.kind = ConstructWrap::Kind::t_pending;
            }
            return result;
        }

        const size_t remaining_n = call.arguments.size() - remaining_start;
        ExprNode *only = remaining_n == 1 ? call.arguments[remaining_start] : nullptr;

        // wrapping an unsettled value that types on its own reads it as constructor operands
        // (`Endpoint<Show>(Endpoint<Show>('/x'))`, `$h->at` of an `M`)
        if (only != nullptr && is_undetermined_type(only->result_type())
            && !types_only_at_destination(only)) {
            result.kind = ConstructWrap::Kind::t_pending;
            return result;
        }

        const bool exact_one = only != nullptr
            && argument_fit(only->result_type(), only, target) == ArgumentFit::t_exact;
        if (exact_one) {
            return result;
        }

        std::vector<ExprNode *> inner_args(
            call.arguments.begin() + static_cast<std::ptrdiff_t>(remaining_start),
            call.arguments.end()
        );
        std::vector<CallArgumentName> inner_names = construct_inner_argument_names(
            call.argument_names,
            remaining_start,
            call.arguments.size(),
            param);

        const auto &ctors = constructors_of(target, functions);
        bool constructor_binds = false;
        for (FunctionDeclNode *ctor : ctors) {
            if (bind_arguments(*ctor, inner_args, inner_names).kind == ArgumentBindKind::t_ok) {
                constructor_binds = true;
                break;
            }
        }

        if (!constructor_binds) {
            if (!target.has_complex_type()) {
                return result;
            }

            for (FunctionDeclNode *ctor : ctors) {
                if (ctor != nullptr && constructor_arity_pending(*ctor, module)) {
                    result.kind = ConstructWrap::Kind::t_pending;
                    return result;
                }
            }

            return ConstructWrap {
                .kind = ConstructWrap::Kind::t_unconstructable,
                .target = target,
                .constructors = { ctors.begin(), ctors.end() },
            };
        }

        auto wrapped = bind_construct_prefix(
            candidate, call.arguments, call.argument_names, remaining_start);
        if (!wrapped.has_value()) {
            return result;
        }

        return ConstructWrap {
            .kind = ConstructWrap::Kind::t_wrap,
            .binding = std::move(*wrapped),
            .target = target,
            .remaining_start = remaining_start,
        };
    }

    void apply_construct_wrap(
        FunctionCallExprNode &call,
        const ConstructWrap &plan,
        NodeCollection &nodes
    )
    {
        if (plan.kind != ConstructWrap::Kind::t_wrap || call.decl == nullptr) {
            return;
        }

        const size_t remaining_start = plan.remaining_start;
        if (remaining_start > call.arguments.size()) {
            return;
        }

        VarDeclNode *param = remaining_start < call.decl->args.size()
            ? call.decl->args[remaining_start] : nullptr;

        std::vector<ExprNode *> inner_args(
            call.arguments.begin() + static_cast<std::ptrdiff_t>(remaining_start),
            call.arguments.end()
        );
        std::vector<CallArgumentName> inner_names = construct_inner_argument_names(
            call.argument_names,
            remaining_start,
            call.arguments.size(),
            param);

        auto &inner = nodes.emplace_back<FunctionCallExprNode>(
            call.token_function_name, std::move(inner_args));
        inner.constructed_type = plan.target;
        inner.argument_names = std::move(inner_names);

        std::vector<ExprNode *> rewritten(
            call.arguments.begin(),
            call.arguments.begin() + static_cast<std::ptrdiff_t>(remaining_start)
        );
        rewritten.push_back(&inner);

        std::vector<CallArgumentName> rewritten_names;
        rewritten_names.reserve(rewritten.size());
        for (size_t i = 0; i < remaining_start; i++) {
            if (i < call.argument_names.size()) {
                rewritten_names.push_back(call.argument_names[i]);
            } else {
                rewritten_names.emplace_back();
            }
        }

        CallArgumentName construct_name {};
        if (param != nullptr && param->has_label()) {
            construct_name.kind = ArgumentNameKind::t_label;
            construct_name.token.emplace(param->label_token.value());
        }
        rewritten_names.push_back(std::move(construct_name));

        call.arguments = std::move(rewritten);
        call.argument_names = std::move(rewritten_names);
    }

    std::optional<std::string> construct_refusal(const FunctionDeclNode &decl)
    {
        const size_t implicit = decl.implicit_arg_count();

        std::optional<size_t> construct_at;
        for (size_t i = implicit; i < decl.args.size(); i++) {
            const VarDeclNode *arg = decl.args[i];
            if (arg == nullptr || !arg->constructs_in_place) {
                continue;
            }

            if (construct_at.has_value()) {
                return "'construct' can only sit on one parameter, the last one.";
            }

            construct_at = i;
        }

        if (!construct_at.has_value()) {
            return std::nullopt;
        }

        const size_t index = *construct_at;
        const VarDeclNode *arg = decl.args[index];

        if (index != decl.args.size() - 1) {
            return "'construct' can only sit on the last parameter: remaining call-site "
                "arguments build this one.";
        }

        if (arg->takes_ownership) {
            return "'construct' cannot combine with 'mv' - the constructed value arrives by value.";
        }

        if (arg->access_effect != AccessEffect::t_none) {
            return "'construct' cannot combine with an access effect - the constructed value "
                "arrives by value.";
        }

        if (!arg->has_type()) {
            return std::nullopt;
        }

        const ValueType type = arg->type();

        if (type.is_type_param() || is_undetermined_type(type)) {
            return std::nullopt;
        }

        if (type.is_pointer()) {
            return "'construct' needs a by-value parameter, not a borrow or a pointer.";
        }

        if (type.is_nullable() || type.is_wrapped_optional()) {
            return "'construct' cannot target an optional - construct the payload and wrap it "
                "in the body.";
        }

        if (type.is_interface()) {
            return "'construct' cannot target an interface - construct a type that answers it.";
        }

        if (type.is_enum()) {
            return "'construct' cannot target an enum - cases are constructed by name, not as "
                "the type itself.";
        }

        if (type.is_void()) {
            return "'construct' cannot target 'void' - it is not a value.";
        }

        if (type.is_struct() || type.is_class() || type.is_primitive()) {
            return std::nullopt;
        }

        return "'construct' can only target a struct, a class, a primitive, or a type parameter.";
    }
};
