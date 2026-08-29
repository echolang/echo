#include "AST/ASTArgumentBind.h"

#include "AST/ASTClone.h"
#include "AST/ASTCodeRef.h"
#include "AST/ASTCollector.h"
#include "AST/ASTFunctionMatcher.h"
#include "AST/ASTIssue.h"
#include "AST/ASTLiteralTyping.h"
#include "AST/ASTSourceToken.h"
#include "AST/FunctionDeclNode.h"
#include "AST/VarDeclNode.h"

#include <fmt/core.h>

#include <cassert>
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
}

AST::ArgumentBinding AST::bind_arguments(
    const AST::FunctionDeclNode &decl,
    const std::vector<AST::ExprNode *> &arguments,
    const std::vector<AST::CallArgumentName> &argument_names
)
{
    ArgumentBinding result;

    const size_t implicit = decl.implicit_arg_count();
    const size_t arity = decl.args.size();

    if (arguments.size() < implicit) {
        result.kind = ArgumentBindKind::t_too_many_positional;
        return result;
    }

    result.filled.assign(arity, BoundArgument {});

    for (size_t i = 0; i < implicit && i < arguments.size(); i++) {
        result.filled[i].value = arguments[i];
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

    for (size_t w = implicit; w < arguments.size(); w++) {
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
    AST::TypeRegistry &registry
)
{
    assert(binding.kind == ArgumentBindKind::t_ok && "applying a refused binding");

    std::vector<ExprNode *> filled;
    filled.reserve(binding.filled.size());

    for (const BoundArgument &slot : binding.filled) {
        if (slot.value == nullptr) {
            filled.push_back(nullptr);
            continue;
        }

        if (!slot.from_default) {
            filled.push_back(slot.value);
            continue;
        }

        AST::TypeSubstitution subst;
        AST::CloneContext cc(nodes, subst, registry);
        filled.push_back(AST::clone_sharing_closures(slot.value, cc));
    }

    call.arguments = std::move(filled);
    call.argument_names.clear();
}

AST::BoundSlots AST::bound_slots(const AST::ArgumentBinding &binding)
{
    BoundSlots result;
    result.types.reserve(binding.filled.size());
    result.exprs.reserve(binding.filled.size());
    result.defers.reserve(binding.filled.size());

    for (const BoundArgument &slot : binding.filled) {
        result.types.push_back(slot.value != nullptr ? slot.value->result_type() : ValueType::make_unknown());
        result.exprs.push_back(slot.value);
        result.defers.push_back(is_untyped_literal(slot.value));
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
