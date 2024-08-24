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

        std::string instance_key(const FunctionDeclNode *tmpl, const std::vector<ValueType> &args)
        {
            std::string key = std::to_string(reinterpret_cast<uintptr_t>(tmpl));
            for (const auto &arg : args) {
                key += "|" + arg.get_mangled_name();
            }
            return key;
        }

        // true if the type still mentions a type parameter (directly, or as an argument of a
        // generic application), i.e. it has not been fully resolved to a concrete type yet.
        bool contains_type_param(const ValueType &type)
        {
            if (type.is_type_param()) {
                return true;
            }
            if (type.is_struct() || type.is_class()) {
                ComplexType *ct = type.get_complex_type();
                if (ct && ct->is_instantiated()) {
                    for (const auto &arg : ct->instantiation_args) {
                        if (contains_type_param(arg)) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }
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

    void Monomorphizer::unify(const ValueType &param, const ValueType &arg, std::vector<ValueType> &out, std::vector<bool> &resolved)
    {
        // a bare type parameter binds directly to the argument type
        if (param.is_type_param()) {
            size_t idx = param.get_type_param_index();
            if (idx < out.size()) {
                out[idx] = arg;
                resolved[idx] = true;
            }
            return;
        }

        // a generic application binds structurally, e.g. Box<T> against Box<int> binds T=int
        if ((param.is_struct() || param.is_class()) && (arg.is_struct() || arg.is_class())) {
            ComplexType *pct = param.get_complex_type();
            ComplexType *act = arg.get_complex_type();
            if (pct && act && pct->is_instantiated() && act->is_instantiated()
                && pct->template_ref == act->template_ref
                && pct->instantiation_args.size() == act->instantiation_args.size()) {
                for (size_t i = 0; i < pct->instantiation_args.size(); i++) {
                    unify(pct->instantiation_args[i], act->instantiation_args[i], out, resolved);
                }
            }
        }
    }

    std::optional<std::vector<ValueType>> Monomorphizer::determine_type_args(FunctionCallExprNode *call, Module &mod, bool &is_error)
    {
        FunctionDeclNode *tmpl = call->decl;
        size_t n = tmpl->type_parameters.size();

        std::vector<ValueType> args;

        if (!call->explicit_type_args.empty()) {
            // explicit type arguments win: foo<int>(...)
            if (call->explicit_type_args.size() != n) {
                _collector.collect_issue<Issue::GenericError>(code_ref_for(mod, call->token_function_name),
                    "Wrong number of type arguments for generic function '" + tmpl->func_name() + "'");
                is_error = true;
                return std::nullopt;
            }
            for (auto *type_node : call->explicit_type_args) {
                args.push_back(type_node->type);
            }
        } else {
            // otherwise infer from the call arguments
            if (call->arguments.size() != tmpl->args.size()) {
                _collector.collect_issue<Issue::GenericError>(code_ref_for(mod, call->token_function_name),
                    "Argument count mismatch for generic function '" + tmpl->func_name() + "'");
                is_error = true;
                return std::nullopt;
            }

            std::vector<ValueType> out(n, ValueType::make_unknown());
            std::vector<bool> resolved(n, false);
            for (size_t i = 0; i < call->arguments.size(); i++) {
                unify(tmpl->args[i]->type(), call->arguments[i]->result_type(), out, resolved);
            }
            for (size_t i = 0; i < n; i++) {
                if (!resolved[i]) {
                    return std::nullopt;
                }
            }
            args = std::move(out);
        }

        // if any argument is still non-concrete, this call lives inside an un-instantiated
        // template body (its type arguments mention the enclosing template's parameters, or an
        // operand's type is still void/unknown). skip it silently and revisit once the enclosing
        // template is instantiated, which substitutes those to concrete types. this guard is why
        // e.g. the inner Box<T>(...) in a generic factory is not instantiated as Box<T> — only as
        // Box<int> after the factory is cloned for int.
        for (const auto &arg : args) {
            if (contains_type_param(arg) || arg.is_void() || arg.get_kind() == ValueTypeKind::t_unknown) {
                return std::nullopt;
            }
        }

        return args;
    }

    FunctionDeclNode *Monomorphizer::get_or_create_function_instance(FunctionDeclNode *tmpl, const std::vector<ValueType> &args)
    {
        std::string key = instance_key(tmpl, args);
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
                        + tmpl->func_name() + "' — likely non-terminating generic instantiation");
                _instance_cap_reported = true;
            }
            return nullptr;
        }

        if (_trace) {
            std::cout << "[mono]   create new instance of '" << tmpl->func_name() << "'" << std::endl;
        }

        // clone the template into its home module, substituting T -> concrete throughout
        CloneContext cc(home->nodes, args, _collector.type_registry);
        auto *instance = static_cast<FunctionDeclNode *>(tmpl->clone(cc));
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
            // wrap an lvalue variable in a VarPtrExprNode when the parameter is a pointer,
            // same as the non-generic path in FuncCallParser (keeps codegen uniform)
            call->arguments[i] = coerce_arg_to_pointer_param(mod.nodes, call->arguments[i], expected);
            ValueType actual = call->arguments[i]->result_type();
            if (!(actual == expected)) {
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
                                    + " rounds — generic '" + call->decl->func_name() + "' could not be resolved");
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
        // "name(int32, float64) -> Box<int32>" for a resolved instance
        std::string describe_signature(const FunctionDeclNode *fn)
        {
            std::string result = fn->func_name() + "(";
            for (size_t i = 0; i < fn->args.size(); i++) {
                if (i > 0) {
                    result += ", ";
                }
                result += fn->args[i]->type().get_type_desciption();
            }
            result += ") -> " + fn->get_return_type().get_type_desciption();
            return result;
        }
    }

    std::string Monomorphizer::debug_dump_instances() const
    {
        // instances created: the concrete clones the fixpoint produced from generic templates
        std::vector<std::string> instance_lines;
        std::unordered_set<const FunctionDeclNode *> instances;
        for (const auto &[key, instance] : _func_instances) {
            instances.insert(instance);
            instance_lines.push_back("- " + describe_signature(instance));
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
        // that still mention a type parameter (Box<T0>) — those are template-body artifacts, not the
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
