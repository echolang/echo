#include "AST/ASTMonomorphizer.h"

#include "AST/ASTClone.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ExprNode.h"
#include "AST/TypeNode.h"
#include "AST/TypeCastNode.h"
#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarRefNode.h"
#include "AST/VarNode.h"
#include "AST/ASTArgumentCoercion.h"
#include "Debugging.h"

#include <fmt/core.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace AST
{
    namespace
    {
        // runaway guards: real programs stay far below these; hitting them means a
        // pathological (e.g. recursively growing) instantiation, which we stop rather than
        // spin on forever.
        constexpr size_t MAX_INSTANCES = 4096;
        constexpr size_t MAX_ROUNDS = 256;

    }

    Monomorphizer::Monomorphizer(Bundle &bundle)
        : _bundle(bundle), _collector(bundle.collector)
    {
        _trace = std::getenv("ECO_TRACE_MONO") != nullptr;
    }

    CodeRef Monomorphizer::code_ref_for(Module &mod, const TokenReference &token)
    {
        const File *file = mod.files().first();
        return CodeRef{&mod, file, token.make_slice()};
    }

    std::optional<std::vector<ValueType>> Monomorphizer::determine_type_args(FunctionCallExprNode *call, Module &mod, bool &is_error)
    {
        FunctionDeclNode *tmpl = call->decl;
        size_t n = tmpl->type_parameters.size();

        std::vector<ValueType> args;

        // a method carries its owner's parameters ahead of its own, and only the *own* ones can be
        // spelled at the call site: `$b->map<float64>()` says nothing about Box's T, which the
        // receiver already fixes. so the two halves are resolved from different places
        const size_t inherited = tmpl->inherited_type_param_count;

        if (!call->explicit_type_args.empty()) {
            // explicit type arguments win: foo<int>(...)
            if (call->explicit_type_args.size() != tmpl->own_type_param_count()) {
                _collector.collect_issue<Issue::GenericError>(code_ref_for(mod, call->token_function_name),
                    "Wrong number of type arguments for generic function '" + tmpl->func_name() + "'");
                is_error = true;
                return std::nullopt;
            }

            // the owner's parameters come from the receiver, which is argument 0. inferred rather
            // than read off the receiver's instantiation_args so there is one binding rule: the
            // receiver parameter is `Box<T>&` and unify already descends a generic application
            TypeSubstitution from_receiver;
            if (inherited > 0 && !call->arguments.empty()) {
                unify_type(tmpl->args[0]->type(), call->arguments[0]->result_type(), from_receiver);
            }

            for (size_t i = 0; i < inherited; i++) {
                const ValueType *bound = from_receiver.lookup(tmpl->type_parameters[i]);

                // the receiver's own type is not concrete yet - this call sits in a template
                // body that has not been instantiated. retried, not reported
                if (!bound) {
                    return std::nullopt;
                }

                args.push_back(*bound);
            }

            for (auto *type_node : call->explicit_type_args) {
                args.push_back(type_node->type);
            }
        } else {
            // otherwise infer from the call arguments
            if (call->arguments.size() != tmpl->args.size()) {
                _collector.collect_issue<Issue::GenericError>(code_ref_for(mod, call->token_function_name),
                    fmt::format("Argument count mismatch for generic function '{}': it takes {}, {} given",
                        tmpl->func_name(),
                        tmpl->args.size() - tmpl->implicit_arg_count(),
                        call->arguments.size() - tmpl->implicit_arg_count()));
                is_error = true;
                return std::nullopt;
            }

            // unify binds in *argument* order; the type-argument list has to come back out in
            // *declaration* order, because that order is what identifies the instantiation
            TypeSubstitution inferred;
            for (size_t i = 0; i < call->arguments.size(); i++) {
                unify_type(tmpl->args[i]->type(), call->arguments[i]->result_type(), inferred);
            }

            // an unbound parameter is only retryable while the call still sits in a template body
            // that has not been instantiated. once every argument type is concrete, nothing later
            // can bind it, so this is a real "cannot infer" rather than a not-yet
            bool arguments_concrete = true;
            for (auto *argument : call->arguments) {
                if (is_undetermined_type(argument->result_type())) {
                    arguments_concrete = false;
                    break;
                }
            }

            for (const auto *param : tmpl->type_parameters) {
                const ValueType *bound = inferred.lookup(param);
                if (!bound) {
                    if (arguments_concrete) {
                        _collector.collect_issue<Issue::UnresolvedTypeParameter>(
                            code_ref_for(mod, call->token_function_name),
                            "Cannot infer type parameter '" + param->name + "' of generic '"
                                + tmpl->func_name() + "' from the call arguments; specify it explicitly, e.g. "
                                + tmpl->func_name() + "<...>(...)");
                        is_error = true;
                    }
                    return std::nullopt;
                }
                args.push_back(*bound);
            }
        }

        // if any argument is still non-concrete, this call lives inside an un-instantiated
        // template body (its type arguments mention the enclosing template's parameters, or an
        // operand's type is still void/unknown). skip it silently and revisit once the enclosing
        // template is instantiated, which substitutes those to concrete types. this guard is why
        // e.g. the inner Box<T>(...) in a generic factory is not instantiated as Box<T> - only as
        // Box<int> after the factory is cloned for int.
        for (const auto &arg : args) {
            if (is_undetermined_type(arg)) {
                return std::nullopt;
            }
        }

        // enforce any type-parameter constraints (e.g. `<T: numeric>`). both the explicit
        // and inferred paths converge on `args`, so this single check covers both.
        for (size_t i = 0; i < n; i++) {
            const auto *param = tmpl->type_parameters[i];
            if (param->is_constrained() && !param->allows(args[i])) {
                _collector.collect_issue<Issue::UnsatisfiedTypeConstraint>(code_ref_for(mod, call->token_function_name),
                    "Type parameter '" + param->name + "' of '" + tmpl->func_name() +
                    "' is constrained to '" + param->constraint_spelling +
                    "' but was given '" + args[i].get_type_desciption() + "'");
                is_error = true;
                return std::nullopt;
            }
        }

        return args;
    }

    FunctionDeclNode *Monomorphizer::get_or_create_function_instance(FunctionDeclNode *tmpl, const std::vector<ValueType> &args)
    {
        InstanceKey key = std::make_tuple(static_cast<const FunctionDeclNode *>(tmpl), args);
        if (auto it = _func_instances.find(key); it != _func_instances.end()) {
            if (_trace) {
                std::cout << "[mono]   reuse existing instance of '" << tmpl->func_name() << "'" << std::endl;
            }
            return it->second;
        }

        auto module_it = _decl_module.find(tmpl);
        if (module_it == _decl_module.end()) {
            return nullptr;
        }
        Module *home = module_it->second;

        // runaway guard: hitting the instance cap means instantiation is not converging (e.g. a
        // generic recursing on an ever-growing type). report it once, located at the template, so
        // it surfaces as a diagnostic instead of a silent stall.
        if (++_instance_count > MAX_INSTANCES) {
            if (!_instance_cap_reported && tmpl->name_token.has_value()) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(*home, tmpl->name_token.value()),
                    "instantiation limit (" + std::to_string(MAX_INSTANCES) + ") hit resolving generic '"
                        + tmpl->func_name() + "': likely non-terminating generic instantiation");
                _instance_cap_reported = true;
            }
            return nullptr;
        }

        if (_trace) {
            std::cout << "[mono]   create new instance of '" << tmpl->func_name() << "'" << std::endl;
        }

        // clone the template into its home module, substituting T -> concrete throughout.
        // an instance must be fully concrete, so every one of the template's parameters has to be
        // bound: positional() asserts the arity, and the loop below asserts nothing is left over
        TypeSubstitution subst = TypeSubstitution::positional(tmpl->type_parameters, args);
        for (const auto *param : tmpl->type_parameters) {
            assert(subst.covers(param) && "instantiating a template with an incomplete substitution");
        }

        CloneContext cc(home->nodes, subst, _collector.type_registry);
        auto *instance = static_cast<FunctionDeclNode *>(tmpl->clone(cc));

        // stamp the instantiation identity onto the instance. the mangled name is built from
        // these, so without them two instantiations of a generic whose parameter shows up only
        // in the return type share one symbol - see FunctionDeclNode::instantiation_args
        instance->template_ref = tmpl;
        instance->instantiation_args = args;

        _func_instances[key] = instance;

        // make the instance visible to codegen, which emits bodies from the file root children
        File *file = home->files().first();
        if (file && file->root) {
            file->root->children.push_back(make_ref(*instance));
        }

        return instance;
    }

    void Monomorphizer::insert_argument_casts(FunctionCallExprNode *call, FunctionDeclNode *instance, Module &mod)
    {
        for (size_t i = 0; i < call->arguments.size() && i < instance->args.size(); i++) {
            ValueType expected = instance->args[i]->type();
            // wrap a place expression in an AddrOfExprNode when the parameter is a borrow,
            // same as the non-generic path in FuncCallParser (keeps codegen uniform)
            call->arguments[i] = coerce_arg_to_pointer_param(mod.nodes, call->arguments[i], expected);
            ValueType actual = call->arguments[i]->result_type();
            // mirrors FuncCallParser: a borrow widening to a nullable pointer needs no cast
            if (!is_implicitly_convertible(actual, expected)) {
                auto &cast = mod.nodes.emplace_back<TypeCastNode>(expected, call->arguments[i], true);
                call->arguments[i] = &cast;
            }
        }
    }

    void Monomorphizer::run()
    {
        // map every function declaration to the module that owns it
        for (auto &module_ptr : _bundle.modules) {
            Module &mod = *module_ptr;
            for (auto *fn : mod.nodes.of_type<FunctionDeclNode>()) {
                _decl_module[fn] = &mod;
            }
        }

        // fixpoint: each round resolves the calls whose type arguments are now concrete;
        // cloning a template exposes its body's calls (with concrete types) for the next round.
        bool progressed = true;
        size_t rounds = 0;
        while (progressed) {
            progressed = false;
            if (++rounds > MAX_ROUNDS) {
                // the fixpoint did not converge. locate the report at any generic call still
                // unresolved so the user has a concrete site to look at rather than a silent stall.
                bool reported = false;
                for (auto &module_ptr : _bundle.modules) {
                    if (reported) {
                        break;
                    }
                    Module &mod = *module_ptr;
                    for (auto *call : mod.nodes.of_type<FunctionCallExprNode>()) {
                        if (call->decl && call->decl->is_generic() && !_processed.count(call)) {
                            _collector.collect_issue<Issue::GenericError>(
                                code_ref_for(mod, call->token_function_name),
                                "monomorphization did not converge after " + std::to_string(MAX_ROUNDS)
                                    + " rounds: generic '" + call->decl->func_name() + "' could not be resolved");
                            reported = true;
                            break;
                        }
                    }
                }
                break;
            }

            // snapshot the current calls; cloning appends new ones as we go
            std::vector<std::pair<FunctionCallExprNode *, Module *>> calls;
            for (auto &module_ptr : _bundle.modules) {
                Module &mod = *module_ptr;
                for (auto *call : mod.nodes.of_type<FunctionCallExprNode>()) {
                    calls.push_back({call, &mod});
                }
            }

            for (auto &[call, mod] : calls) {
                if (_processed.count(call)) {
                    continue;
                }
                if (!call->decl || !call->decl->is_generic()) {
                    _processed.insert(call);
                    continue;
                }

                bool is_error = false;
                auto args = determine_type_args(call, *mod, is_error);
                if (!args) {
                    // reported errors are final; unresolved template-body calls retry later
                    if (is_error) {
                        _processed.insert(call);
                    }
                    continue;
                }

                _processed.insert(call);

                if (_trace) {
                    std::string arg_desc;
                    for (size_t i = 0; i < args->size(); i++) {
                        arg_desc += (i > 0 ? ", " : "") + (*args)[i].get_type_desciption();
                    }
                    std::cout << "[mono] round " << rounds << ": resolve '" << call->decl->func_name()
                              << "' with <" << arg_desc << ">" << std::endl;
                }

                FunctionDeclNode *instance = get_or_create_function_instance(call->decl, *args);
                if (instance) {
                    call->decl = instance;
                    insert_argument_casts(call, instance, *mod);
                    progressed = true;
                }
            }

            // a variable whose type was inferred from a generic call (e.g. `$b = Box<int>(...)`)
            // captured the template's un-substituted return type at parse time; now that the
            // call points at the concrete instance, re-derive the variable's type from its
            // initializer. this can unblock calls that take the variable as an argument (e.g.
            // unwrap($b)), so it runs inside the fixpoint and reports progress.
            for (auto &module_ptr : _bundle.modules) {
                Module &mod = *module_ptr;
                for (auto *decl : mod.nodes.of_type<VarDeclNode>()) {
                    if (!decl->has_type() || !decl->init_expr) {
                        continue;
                    }
                    if (!contains_type_param(decl->type())) {
                        continue;
                    }
                    ValueType derived = decl->init_expr->result_type();
                    if (contains_type_param(derived)) {
                        continue;  // initializer is not concrete yet either; revisit next round
                    }
                    auto &type_node = mod.nodes.emplace_back<TypeNode>(derived);
                    decl->set_type_node(&type_node);
                    progressed = true;
                }
            }
        }
    }

    namespace
    {
        // "name(int32, float64) -> Box<int32>" for a resolved instance. the signature itself is the
        // declaration's own rendering; only the return-type suffix belongs to this dump
        std::string describe_signature(const FunctionDeclNode *fn)
        {
            return fn->signature_description() + " -> " + fn->get_return_type().get_type_desciption();
        }
    }

    std::string Monomorphizer::debug_dump_instances() const
    {
        // instances created: the concrete clones the fixpoint produced from generic templates
        std::vector<std::string> instance_lines;
        std::unordered_set<const FunctionDeclNode *> instances;
        for (const auto &[key, instance] : _func_instances) {
            instances.insert(instance);

            // the key carries what the signature alone cannot: which template this came from and
            // which parameter took which type. without it a dump of `Box(int32) -> Box<int32>`
            // never says it is an instance of Box<T> at all
            const FunctionDeclNode *tmpl = std::get<0>(key);
            const std::vector<ValueType> &args = std::get<1>(key);

            std::string bindings;
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) {
                    bindings += ", ";
                }
                bindings += (i < tmpl->type_parameters.size() ? tmpl->type_parameters[i]->name : "?")
                    + " = " + args[i].get_type_desciption();
            }

            instance_lines.push_back("- " + describe_signature(instance)
                + "  [" + tmpl->func_name() + "<" + bindings + ">]");
        }

        // rewired call sites: calls whose decl now points at one of those instances
        std::vector<std::string> call_lines;
        for (auto &module_ptr : _bundle.modules) {
            Module &mod = *module_ptr;
            for (auto *call : mod.nodes.of_type<FunctionCallExprNode>()) {
                if (!call->decl || !instances.count(call->decl)) {
                    continue;
                }
                std::string line = "- " + call->token_function_name.value() + "(";
                for (size_t i = 0; i < call->arguments.size(); i++) {
                    if (i > 0) {
                        line += ", ";
                    }
                    line += call->arguments[i]->result_type().get_type_desciption();
                }
                line += ") => " + describe_signature(call->decl);
                call_lines.push_back(line);
            }
        }

        // struct instantiations: the concrete ComplexTypes interned by the type registry. skip any
        // that still mention a type parameter (Box<T>) - those are template-body artifacts, not the
        // concrete instantiations (Box<int32>) the user cares about.
        std::vector<std::string> struct_lines;
        for (const ComplexType *ct : _collector.type_registry.instantiations()) {
            bool concrete = true;
            for (const auto &arg : ct->instantiation_args) {
                if (contains_type_param(arg)) {
                    concrete = false;
                    break;
                }
            }
            if (concrete) {
                struct_lines.push_back("- " + ct->name.value_or("[anonymous]"));
            }
        }

        // deterministic output so the dump can be diffed and cross-checked against -a
        std::sort(instance_lines.begin(), instance_lines.end());
        std::sort(call_lines.begin(), call_lines.end());
        std::sort(struct_lines.begin(), struct_lines.end());

        auto section = [](const std::string &title, const std::vector<std::string> &lines) {
            std::string body = lines.empty() ? "  (none)\n" : "";
            for (const auto &line : lines) {
                body += DD::tabbify(line, 2) + "\n";
            }
            return title + "\n" + body;
        };

        std::string result = "Monomorphization instances\n{\n";
        result += DD::tabbify(section("Instances created:", instance_lines), 2);
        result += DD::tabbify(section("Rewired call sites:", call_lines), 2);
        result += DD::tabbify(section("Struct instantiations:", struct_lines), 2);
        result += "}\n";
        return result;
    }
}
