#include "AST/ASTArgumentBind.h"

#include "AST/ASTArgumentFit.h"
#include "AST/ASTArrayLiteral.h"
#include "AST/ASTBuiltin.h"
#include "AST/ASTClone.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTCodeRef.h"
#include "AST/ASTCollectionLiteral.h"
#include "AST/ASTCollector.h"
#include "AST/ASTFunctionMatcher.h"
#include "AST/ASTIssue.h"
#include "AST/ASTLiteralTyping.h"
#include "AST/ASTMapLiteral.h"
#include "AST/ASTSourceToken.h"
#include "AST/ASTTypeUnify.h"
#include "AST/ASTVariadic.h"
#include "AST/FunctionDeclNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"

#include <fmt/core.h>

#include <cassert>
#include <optional>
#include <unordered_map>

namespace
{
    const AST::CallArgumentName k_positional {};

    const AST::CallArgumentName &name_at(
        const std::vector<AST::CallArgumentName> &names,
        size_t index
    )
    {
        if (index < names.size()) {
            return names[index];
        }

        return k_positional;
    }

    std::string written_name(const AST::CallArgumentName &name)
    {
        if (!name.token.has_value()) {
            return "";
        }

        const std::string spelling = name.token->value();

        if (name.kind == AST::ArgumentNameKind::t_parameter && !spelling.empty() && spelling[0] == '$') {
            return spelling.substr(1);
        }

        return spelling;
    }

    AST::FunctionCallExprNode *as_call(AST::ExprNode *expr)
    {
        if (expr == nullptr || expr->get_node_type() != AST::NodeType::n_expr_call) {
            return nullptr;
        }

        return static_cast<AST::FunctionCallExprNode *>(expr);
    }

    bool is_source_here_call(AST::ExprNode *expr)
    {
        AST::FunctionCallExprNode *call = as_call(expr);
        if (call == nullptr || call->decl == nullptr || !call->decl->is_builtin()
            || !call->decl->builtin.has_value()) {
            return false;
        }

        return AST::builtin_kind_for(call->decl->builtin.value()) == AST::BuiltinKind::t_source_here;
    }

    bool param_is_source_location(
        const AST::FunctionCallExprNode &call,
        size_t i,
        AST::Collector &collector
    )
    {
        if (call.decl == nullptr || i >= call.decl->args.size() || call.decl->args[i] == nullptr) {
            return false;
        }
        if (!collector.core_types.has(AST::CoreTypeKind::t_source_location)) {
            return false;
        }

        AST::ValueType have = call.decl->args[i]->type();
        AST::ValueType want = collector.core_types.type(AST::CoreTypeKind::t_source_location);
        if (have.is_pointer()) {
            have = AST::value_type_of(have);
        }

        return AST::ValueType::make_mutable(have) == AST::ValueType::make_mutable(want);
    }

    AST::ExprNode *plant_here_at(
        AST::FunctionCallExprNode &call,
        bool from_default,
        AST::NodeCollection &nodes,
        AST::Collector &collector
    )
    {
        AST::FunctionDeclNode *here = collector.core_types.source_here();
        if (here == nullptr) {
            return nullptr;
        }

        auto &planted = nodes.emplace_back<AST::FunctionCallExprNode>(
            call.token_function_name, std::vector<AST::ExprNode *>{});
        planted.decl = here;
        planted.settlement = AST::CallSettlement::t_settled;
        planted.is_implcit = true;
        planted.from_parameter_default = from_default;
        planted.enclosing_function = call.enclosing_function;
        return &planted;
    }

    // a cloned/defaulted here() is replanted at this call. an explicit here() argument is left:
    // die($msg, std::src::here()) inside #[caller] names this die, not the outer caller
    AST::ExprNode *retarget_defaulted_here(
        AST::FunctionCallExprNode &call,
        size_t i,
        AST::ExprNode *arg,
        bool from_default,
        AST::NodeCollection &nodes,
        AST::Collector &collector
    )
    {
        if (arg == nullptr || !param_is_source_location(call, i, collector)) {
            return nullptr;
        }
        if (!(from_default || arg->from_parameter_default)) {
            return nullptr;
        }
        if (!is_source_here_call(arg)) {
            return nullptr;
        }

        return plant_here_at(call, true, nodes, collector);
    }
}

AST::ArgumentBinding AST::bind_arguments(
    const AST::FunctionDeclNode &decl,
    const std::vector<AST::ExprNode *> &arguments,
    const std::vector<AST::CallArgumentName> &argument_names
)
{
    ArgumentBinding result;

    const size_t written_implicit = decl.written_implicit_arg_count();
    const size_t implicit = decl.implicit_arg_count();
    const size_t arity = decl.args.size();

    if (arguments.size() < written_implicit) {
        result.kind = ArgumentBindKind::t_too_many_positional;
        return result;
    }

    result.filled.assign(arity, BoundArgument {});

    for (size_t i = 0; i < written_implicit && i < arguments.size(); i++) {
        result.filled[i].value = arguments[i];
    }

    if (decl.tracks_caller && written_implicit < arity) {
        result.filled[written_implicit].from_caller = true;
        if (argument_names.empty() && arguments.size() == arity) {
            result.filled[written_implicit].value = arguments[written_implicit];
        }
    }

    std::unordered_map<std::string, size_t> by_parameter;
    std::unordered_map<std::string, size_t> by_label;

    for (size_t i = implicit; i < arity; i++) {
        AST::VarDeclNode *param = decl.args[i];
        if (param == nullptr) {
            continue;
        }

        by_parameter[param->name()] = i;

        if (param->has_label()) {
            by_label[param->label()] = i;
        }
    }

    std::vector<bool> used(arity, false);
    for (size_t i = 0; i < implicit; i++) {
        used[i] = true;
    }

    size_t next_positional = implicit;

    for (size_t w = written_implicit; w < arguments.size(); w++) {
        const CallArgumentName &name = name_at(argument_names, w);

        if (name.kind == ArgumentNameKind::t_none) {
            while (next_positional < arity && used[next_positional]) {
                next_positional++;
            }

            if (next_positional >= arity) {
                result.kind = ArgumentBindKind::t_too_many_positional;
                if (arguments[w] != nullptr) {
                    if (const TokenReference *tok = AST::source_token_of(*arguments[w])) {
                        result.blame_token.emplace(*tok);
                    }
                }
                return result;
            }

            AST::VarDeclNode *param = decl.args[next_positional];
            if (param != nullptr && param->has_label()) {
                result.kind = ArgumentBindKind::t_missing_label;
                result.blame_name = param->label();
                result.missing_param = next_positional;
                if (arguments[w] != nullptr) {
                    if (const TokenReference *tok = AST::source_token_of(*arguments[w])) {
                        result.blame_token.emplace(*tok);
                    }
                }
                return result;
            }

            used[next_positional] = true;
            result.filled[next_positional].value = arguments[w];
            next_positional++;
            continue;
        }

        const std::string key = written_name(name);

        if (name.kind == ArgumentNameKind::t_label) {
            const auto found = by_label.find(key);
            if (found == by_label.end()) {
                result.kind = ArgumentBindKind::t_unknown_name;
                result.blame_name = key;
                if (name.token.has_value()) {
                    result.blame_token.emplace(name.token.value());
                }
                return result;
            }

            const size_t slot = found->second;
            if (used[slot]) {
                result.kind = ArgumentBindKind::t_duplicate_slot;
                result.blame_name = key;
                if (name.token.has_value()) {
                    result.blame_token.emplace(name.token.value());
                }
                return result;
            }

            used[slot] = true;
            result.filled[slot].value = arguments[w];
            continue;
        }

        const auto found = by_parameter.find(key);
        if (found == by_parameter.end()) {
            result.kind = ArgumentBindKind::t_unknown_name;
            result.blame_name = "$" + key;
            if (name.token.has_value()) {
                result.blame_token.emplace(name.token.value());
            }
            return result;
        }

        const size_t slot = found->second;
        AST::VarDeclNode *param = decl.args[slot];

        if (param != nullptr && param->has_label()) {
            result.kind = ArgumentBindKind::t_missing_label;
            result.blame_name = param->label();
            result.missing_param = slot;
            if (name.token.has_value()) {
                result.blame_token.emplace(name.token.value());
            }
            return result;
        }

        if (used[slot]) {
            result.kind = ArgumentBindKind::t_duplicate_slot;
            result.blame_name = "$" + key;
            if (name.token.has_value()) {
                result.blame_token.emplace(name.token.value());
            }
            return result;
        }

        used[slot] = true;
        result.filled[slot].value = arguments[w];
    }

    for (size_t i = implicit; i < arity; i++) {
        if (used[i]) {
            continue;
        }

        AST::VarDeclNode *param = decl.args[i];
        if (param != nullptr && param->init_expr != nullptr) {
            result.filled[i].value = param->init_expr;
            result.filled[i].from_default = true;
            used[i] = true;
            continue;
        }

        result.kind = ArgumentBindKind::t_missing_required;
        result.missing_param = i;
        if (param != nullptr) {
            result.blame_name = param->has_label() ? param->label() : param->name_full();
            result.blame_token.emplace(param->token_varname);
        }
        return result;
    }

    return result;
}

void AST::apply_argument_binding(
    AST::FunctionCallExprNode &call,
    const AST::ArgumentBinding &binding,
    AST::NodeCollection &nodes,
    AST::Collector &collector
)
{
    assert(binding.kind == ArgumentBindKind::t_ok && "applying a refused binding");

    std::vector<ExprNode *> filled;
    filled.reserve(binding.filled.size());

    for (size_t i = 0; i < binding.filled.size(); i++) {
        const BoundArgument &slot = binding.filled[i];
        if (slot.from_caller) {
            if (slot.value != nullptr) {
                filled.push_back(slot.value);
                continue;
            }

            AST::FunctionDeclNode *enclosing = call.enclosing_function;
            if (enclosing != nullptr && enclosing->tracks_caller
                && enclosing->caller_arg_index() < enclosing->args.size()) {
                AST::VarDeclNode *param = enclosing->args[enclosing->caller_arg_index()];
                if (param != nullptr) {
                    auto &var = nodes.emplace_back<AST::VarNode>(
                        param, call.token_function_name);
                    auto &ref = nodes.emplace_back<AST::VarRefNode>(&var);
                    ref.is_implcit = true;
                    filled.push_back(&ref);
                    continue;
                }
            }

            filled.push_back(plant_here_at(call, false, nodes, collector));
            continue;
        }

        if (slot.value == nullptr) {
            filled.push_back(nullptr);
            continue;
        }

        if (AST::ExprNode *here = retarget_defaulted_here(
                call, i, slot.value, slot.from_default, nodes, collector)) {
            filled.push_back(here);
            continue;
        }

        if (!slot.from_default) {
            filled.push_back(slot.value);
            continue;
        }

        AST::TypeSubstitution subst;
        AST::CloneContext cc(nodes, subst, collector.type_registry);
        ExprNode *cloned = AST::clone_sharing_closures(slot.value, cc);
        cloned->from_parameter_default = true;
        filled.push_back(cloned);
    }

    call.arguments = std::move(filled);
    call.argument_names.clear();
}

void AST::retarget_here_defaults(
    AST::FunctionCallExprNode &call,
    AST::NodeCollection &nodes,
    AST::Collector &collector
)
{
    if (call.decl == nullptr) {
        return;
    }

    for (size_t i = 0; i < call.arguments.size(); i++) {
        if (ExprNode *here = retarget_defaulted_here(
                call, i, call.arguments[i], false, nodes, collector)) {
            call.arguments[i] = here;
        }
    }
}

AST::BoundSlots AST::bound_slots(
    const AST::FunctionDeclNode &decl,
    const AST::ArgumentBinding &binding,
    AST::Collector &collector
)
{
    BoundSlots result;
    result.types.reserve(binding.filled.size());
    result.exprs.reserve(binding.filled.size());
    result.defers.reserve(binding.filled.size());
    result.rejected.reserve(binding.filled.size());

    for (size_t i = 0; i < binding.filled.size(); i++) {
        const BoundArgument &slot = binding.filled[i];
        const VarDeclNode *param = i < decl.args.size() ? decl.args[i] : nullptr;

        ValueType type = ValueType::make_unknown();
        bool rejected = false;
        bool defers = is_untyped_literal(slot.value);

        if ((slot.from_default || slot.from_caller) && param != nullptr && param->has_type()
            && !contains_type_param(param->type())) {
            // a generic template's default is often still untyped; the parameter is concrete
            // (`map<string, string>`). a parameter that still names `T` must not score here:
            // `Engine<string>()` would stay pending on `array<M>`
            type = param->type();
        }
        else if (!slot.from_default && !slot.from_caller && param != nullptr && param->has_type()) {
            CollectionLiteralExprNode *literal = collection_literal_of(slot.value);

            if (literal != nullptr && !literal->expansion_decided) {
                // a pack stays unknown: ranking waits on the elements, not the list
                if (!is_variadic_args(param->type(), collector.core_types)) {
                    const ValueType dest = implicit_conversion_target(param->type());

                    if (!contains_type_param(dest)) {
                        if (std::optional<CollectionBind> bound =
                                collection_bind_type(param->type(), collector, *literal)) {
                            if (collection_fill_rejects(bound->fill, collector, *literal)) {
                                rejected = true;
                            } else {
                                // ranking-only: do not write bound_type, or two candidates fight
                                // over one literal. conversion dest scores the source so
                                // argument_fit ranks t_declared_conversion, not exact-at-V
                                type = bound->type;
                            }
                        }
                        else if (type_is_determined_for_fit(dest)) {
                            rejected = true;
                        }
                    }
                    else {
                        // **a parameter that still names `T` cannot type the literal, and the literal
                        // is the only thing that could bind `T`** - `size<T>(const array<T>&)` called as
                        // `size([3, 1])` waited on both forever, and the body around it was never owned.
                        // so it scores at the type its elements give it, the one `$a = [3, 1]` declares
                        // (core array/map, never `T[N]` or a user `[]=`), and defers like an untyped
                        // scalar: a typed sibling still decides `T`, and the literal is filled at
                        // whatever the instance's parameter says
                        const CollectionLiteralLookup own = collection_literal_type_for(
                            *literal, collector.core_types, collector.type_registry);

                        // a deferred unify failure is silent in can_instantiate, so a shape that
                        // does not match is `rejected` here: an `array<int32>` is never a `map<K, V>`
                        // pending and empty (`[]`) leave the slot unknown, so `pair([], $wide)` still
                        // binds `T` from the sibling
                        if (own.result == CollectionLiteralLookup::Result::t_ok) {
                            TypeSubstitution fresh;

                            if (unify_type(param->type(), own.type, fresh)) {
                                type = own.type;
                                defers = true;
                            }
                            else {
                                rejected = true;
                            }
                        }
                    }
                }
            }
            else if (slot.value != nullptr) {
                type = slot.value->result_type();
            }
        }
        else if (slot.value != nullptr) {
            type = slot.value->result_type();
        }

        result.types.push_back(type);
        result.exprs.push_back(slot.value);
        result.defers.push_back(defers);
        result.rejected.push_back(rejected);
    }

    return result;
}

void AST::report_argument_bind_failure(
    AST::Collector &collector,
    const AST::FunctionCallExprNode &call,
    const std::vector<AST::FunctionDeclNode *> &candidates,
    const AST::CodeRef &at
)
{
    if (candidates.empty()) {
        return;
    }

    const std::string &name = call.token_function_name.value();

    std::vector<ArgumentBinding> failures;
    failures.reserve(candidates.size());

    for (const FunctionDeclNode *candidate : candidates) {
        failures.push_back(bind_arguments(*candidate, call.arguments, call.argument_names));
    }

    const ArgumentBindKind kind = failures.front().kind;
    bool all_same = true;

    for (size_t i = 1; i < failures.size(); i++) {
        if (failures[i].kind != kind) {
            all_same = false;
            break;
        }
    }

    const ArgumentBinding &sample = failures.front();
    const bool specific = candidates.size() == 1
        || (all_same && kind != ArgumentBindKind::t_too_many_positional && kind != ArgumentBindKind::t_ok);
    const CodeRef where = (sample.kind == ArgumentBindKind::t_unknown_name
            || sample.kind == ArgumentBindKind::t_duplicate_slot)
        ? at_token(at, sample.blame_token)
        : at;

    if (specific) {
        switch (sample.kind) {
        case ArgumentBindKind::t_unknown_name:
            collector.collect_issue<Issue::UnknownArgumentName>(where, fmt::format(
                "The argument '{}' does not match any parameter of '{}'.",
                sample.blame_name, name));
            return;
        case ArgumentBindKind::t_duplicate_slot:
            collector.collect_issue<Issue::DuplicateArgumentName>(where, fmt::format(
                "The argument '{}' is specified more than once.",
                sample.blame_name));
            return;
        case ArgumentBindKind::t_missing_label:
            collector.collect_issue<Issue::MissingArgumentLabel>(where, fmt::format(
                "The parameter '{}' of '{}' must be passed as '{}:'.",
                sample.blame_name, name, sample.blame_name));
            return;
        case ArgumentBindKind::t_missing_required:
            collector.collect_issue<Issue::MissingArgument>(where, fmt::format(
                "The parameter '{}' of '{}' has no argument, and no default.",
                sample.blame_name, name));
            return;
        case ArgumentBindKind::t_too_many_positional:
            break;
        case ArgumentBindKind::t_ok:
            break;
        }
    }

    collector.collect_issue<Issue::NoMatchingOverload>(at, fmt::format(
        "No overload of '{}' accepts these arguments. Candidates are:{}",
        name, describe_candidates(candidates)));
}
