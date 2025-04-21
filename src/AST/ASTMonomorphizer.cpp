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
#include "AST/ASTCallResolution.h"
#include "AST/ASTOwnership.h"
#include "AST/ASTPlaceExpr.h"
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
        // spin on forever
        constexpr size_t MAX_INSTANCES = 4096;
        constexpr size_t MAX_ROUNDS = 256;

    }

    Monomorphizer::Monomorphizer(Bundle &bundle)
        : _bundle(bundle), _collector(bundle.collector), _ownership(bundle), _operators(bundle),
          _foreach(bundle)
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
        const FunctionDeclNode *tmpl = call->decl;

        // the inference rules themselves are AST::can_instantiate's, which the parser scores
        // candidates with. everything below is the half the parser must *not* do: a template that
        // cannot take a call is an overload filter there and a diagnostic here, and this is the only
        // place that difference lives
        const Instantiation inst = can_instantiate(tmpl, *call);

        switch (inst.blame) {
        case InstantiationBlame::t_type_argument_count:
            _collector.collect_issue<Issue::GenericError>(code_ref_for(mod, call->token_function_name),
                "Wrong number of type arguments for generic function '" + tmpl->func_name() + "'");
            is_error = true;
            return std::nullopt;

        case InstantiationBlame::t_argument_count:
            _collector.collect_issue<Issue::GenericError>(code_ref_for(mod, call->token_function_name),
                fmt::format("Argument count mismatch for generic function '{}': it takes {}, {} given",
                    tmpl->func_name(),
                    tmpl->args.size() - tmpl->implicit_arg_count(),
                    call->arguments.size() - tmpl->implicit_arg_count()));
            is_error = true;
            return std::nullopt;

        case InstantiationBlame::t_unbound_parameter:
            // nothing binds this parameter and nothing later will, so it is a real "cannot infer"
            // rather than a not-yet
            _collector.collect_issue<Issue::UnresolvedTypeParameter>(
                code_ref_for(mod, call->token_function_name),
                "Cannot infer type parameter '" + inst.param->name + "' of generic '"
                    + tmpl->func_name() + "' from the call arguments; specify it explicitly, e.g. "
                    + tmpl->func_name() + "<...>(...)");
            is_error = true;
            return std::nullopt;

        case InstantiationBlame::t_constraint:
            _collector.collect_issue<Issue::UnsatisfiedTypeConstraint>(code_ref_for(mod, call->token_function_name),
                "Type parameter '" + inst.param->name + "' of '" + tmpl->func_name() +
                "' is constrained to '" + inst.param->constraint_spelling +
                "' but was given '" + inst.bound.get_type_desciption() + "'");
            is_error = true;
            return std::nullopt;

        case InstantiationBlame::t_undecided_parameter:
            // this call lives inside an un-instantiated template body: its type arguments mention
            // the enclosing template's parameters, or a receiver is not resolved. skip it silently
            // and revisit once that template is instantiated, which substitutes them to concrete
            // types. this is why the inner Box<T>(...) of a generic factory is never instantiated as
            // Box<T> - only as Box<int32> after the factory is cloned for int32
            return std::nullopt;

        case InstantiationBlame::t_argument_shape:
            // an argument no substitution can make fit, which is AST::TypeChecker's diagnostic
            // against the instance rather than ours - it numbers the argument and names both types.
            // so fall through and instantiate, if the other parameters decided an instance
            break;

        case InstantiationBlame::n_none:
            break;
        }

        if (!inst.decided) {
            return std::nullopt;
        }

        return inst.type_arguments;
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
        // it surfaces as a diagnostic instead of a silent stall
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

    // every call in the bundle, snapshotted. cloning a template appends new call nodes to the very
    // collections this walks, so the list has to be taken before anything is instantiated
    std::vector<std::pair<FunctionCallExprNode *, Module *>> Monomorphizer::snapshot_calls()
    {
        std::vector<std::pair<FunctionCallExprNode *, Module *>> calls;

        for (auto &module_ptr : _bundle.modules) {
            Module &mod = *module_ptr;
            for (auto *call : mod.nodes.of_type<FunctionCallExprNode>()) {
                calls.push_back({call, &mod});
            }
        }

        return calls;
    }

    // step A: a call naming a template becomes a call naming a concrete instance
    //
    // only the declaration is rewired here. fitting the arguments to it is step C's, through the one
    // implementation the parser also uses - this used to keep a second copy of that loop, and the two
    // disagreed about exactly the case B23 is: the parser's ran for a concrete callee whose arguments
    // were not typed yet, this one only ever ran after substitution
    bool Monomorphizer::instantiate_generic_calls(size_t round)
    {
        bool progressed = false;

        for (auto &[call, mod] : snapshot_calls()) {
            if (_processed.count(call) || !call->decl || !call->decl->is_generic()) {
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
                std::cout << "[mono] round " << round << ": resolve '" << call->decl->func_name()
                          << "' with <" << arg_desc << ">" << std::endl;
            }

            FunctionDeclNode *instance = get_or_create_function_instance(call->decl, *args);
            if (instance) {
                call->decl = instance;
                progressed = true;
            }
        }

        return progressed;
    }

    // step B: a variable whose type was inferred from a call that had not resolved yet
    //
    // `$b = Box<int32>(...)` captured the template's un-substituted return type at parse time, and
    // `$x = f(...)` on a call the parser could not settle captured void. now that those calls point
    // somewhere concrete, re-derive from the initializer. this is what unblocks a call that takes the
    // variable as an argument, which is why it runs inside the fixpoint and reports progress
    bool Monomorphizer::rederive_stale_variable_types()
    {
        bool progressed = false;

        for (auto &module_ptr : _bundle.modules) {
            Module &mod = *module_ptr;
            for (auto *decl : mod.nodes.of_type<VarDeclNode>()) {
                if (!decl->has_type() || !decl->init_expr) {
                    continue;
                }

                // is_undetermined_type rather than contains_type_param: an unresolved call answers
                // void, so a variable initialized from one is stale in exactly the same way as one
                // that captured a `T`, and reads as undetermined for exactly the same reason
                if (!is_undetermined_type(decl->type())) {
                    continue;
                }

                // through the same rule the parser inferred with, rather than a second spelling of
                // it: the value the initializer reads as, and the `const` the declaration was
                // written with - which is still on the stale type, since make_const survives the
                // initializer's type being undetermined
                const ValueType derived =
                    infer_declaration_type(*decl->init_expr, decl->type().is_const());

                if (is_undetermined_type(derived)) {
                    continue;  // initializer is not concrete yet either; revisit next round
                }

                auto &type_node = mod.nodes.emplace_back<TypeNode>(derived);
                decl->set_type_node(&type_node);
                progressed = true;
            }
        }

        return progressed;
    }

    // step B2: a closure environment property whose type came from a variable that was not typed yet
    //
    // capture is decided in the parser, at the read, so the property took the captured variable's type as
    // it stood *then* - which for `$b = Box<int32>(5)` was the template's un-substituted `Box<T>`, stale
    // for precisely the reason step B's variables are. re-derived from the place the capture reads rather
    // than from a remembered type: `captured_values[i]` is what fills the property, so the two cannot
    // drift - and it answers for a capture of any shape, which the declaration behind it would not
    // (a transitive capture, todo/A27 §3, reads the enclosing environment's property, not a local)
    //
    // immediately after step B, so a variable that round made concrete retypes its capture in the same
    // round, and before step C, so the body's `$__env->b->get()` resolves against `Box<int32>` rather
    // than finding only the template's method and reaching codegen unresolved
    //
    // one environment per closure *expression*, which a clone shares (ClosureExprNode::clone) - fine only
    // while a closure cannot be written in a generic body, since two instances' captures would otherwise
    // want two layouts and retype each other every round. that restriction lifts together with the
    // per-instantiation environment todo/A27 §4 describes
    bool Monomorphizer::rederive_stale_capture_types()
    {
        bool progressed = false;

        for (auto &module_ptr : _bundle.modules) {
            for (auto *closure : module_ptr->nodes.of_type<ClosureExprNode>()) {
                ComplexType *environment = closure->environment_type;

                if (environment == nullptr) {
                    continue;  // nothing was captured, so there are no properties to retype
                }

                // by index: the capture list and the property list are built in one order, which is
                // what lets a store in gen_closure_expr find its slot
                for (size_t i = 0; i < closure->captured_values.size(); i++) {
                    if (closure->captured_values[i] == nullptr) {
                        continue;
                    }

                    // the place's own type, *not* value_result_type: a place read into a destination of
                    // the same shape keeps that shape, which is how a captured `int32&` stays a copy of
                    // the reference rather than of the pointee - and it is the type the parse site froze
                    const ValueType derived = closure->captured_values[i]->result_type();

                    if (is_undetermined_type(derived) || derived == environment->get_property_type(i)) {
                        continue;  // not concrete yet, or already what the property says
                    }

                    environment->set_property_type(i, derived);
                    progressed = true;
                }
            }
        }

        return progressed;
    }

    // step C: choose the declaration for any call that still has none, and fit the arguments of every
    // call whose arguments are finally typed
    //
    // after step B, so a local that step A made concrete *this* round is a determined argument in the
    // same round - and before ownership, so no body is walked with a call whose arguments have not
    // been fitted yet
    bool Monomorphizer::settle_calls()
    {
        CallResolver resolver(_collector);
        bool progressed = false;

        for (auto &[call, mod] : snapshot_calls()) {
            // a settled call owes nothing, and a failed one is decided on types no later round can
            // change - re-deriving its match would re-derive its diagnostic with it
            if (call_is_terminal(call->settlement)) {
                continue;
            }

            const auto result = resolver.settle(
                *call, mod->nodes, code_ref_for(*mod, call->token_function_name), false);

            if (result == CallResolver::Result::t_settled) {
                progressed = true;
            }
        }

        return progressed;
    }

    void Monomorphizer::report_unknown_name(FunctionCallExprNode &call, const CodeRef &at)
    {
        // which of the two errors it is, is which kind of call it is - and that is `lookup_namespace`,
        // the one field that distinguishes them: a member call has none, because its candidates come
        // from the receiver sitting in argument 0
        if (call.lookup_namespace == nullptr
            && !call.arguments.empty() && call.arguments[0] != nullptr) {

            const ValueType receiver = target_type_of(call.arguments[0]->result_type());

            // **a receiver that still mentions a type parameter is a template's, not a program's.**
            // this call sits in an un-instantiated body, and the clones the fixpoint made are what
            // carry the concrete receiver - so `$c->count()` inside `f<T>(T& $c)` is answered by
            // `f<Array<int32>>`'s copy and reporting the template's would blame the one body that
            // was never going to be emitted
            //
            // the same silence determine_type_args keeps for t_undecided_parameter, and the same
            // silence the generic-decl arm in finalize_calls keeps one branch up
            if (is_undetermined_type(receiver)) {
                return;
            }

            _collector.collect_issue<Issue::UnknownMember>(
                at, call.token_function_name.value(), receiver.get_type_desciption());
            return;
        }

        _collector.collect_issue<Issue::UnknownFunction>(at, call.token_function_name.value());
    }

    void Monomorphizer::finalize_calls()
    {
        CallResolver resolver(_collector);

        for (auto &[call, mod] : snapshot_calls()) {
            // t_failed among the terminal states is what keeps this sweep from reporting a second
            // time on a call some round already reported - it used to rely on the collector
            // de-duplicating the identical message
            if (call_is_terminal(call->settlement)) {
                continue;
            }

            const CodeRef at = code_ref_for(*mod, call->token_function_name);

            // a call still naming a template is one inside a template nobody instantiated. silent,
            // exactly as determine_type_args is about it: the body is never emitted, and reporting it
            // would blame a declaration for going unused
            if (call->decl != nullptr && call->decl->is_generic()) {
                continue;
            }

            // no declaration after every round: genuinely undecidable, and now it *is* an error.
            // reported here rather than at the parse, because only being out of rounds proves that
            // nothing was going to answer the argument types. Collector::collect_issue de-duplicates
            // on (kind, token range, message), so a template body's call and each of its clones
            // report once between them
            if (call->decl == nullptr) {
                // **`t_unknown_name` is deliberately not reported by settle()** - "no such function"
                // and "no such member" are different errors at different tokens, so each caller words
                // its own. this sweep is the last caller, and it is the only one that can word a
                // *member* call whose receiver never became concrete: `parse_member_call` defers
                // those rather than reporting a receiver type the fixpoint had not answered yet,
                // which is what makes `$views[0]->count()` - an element access whose contract the
                // rewriter attaches mid-fixpoint - resolve at all
                if (resolver.settle(*call, mod->nodes, at, true) == CallResolver::Result::t_unknown_name) {
                    report_unknown_name(*call, at);
                }

                continue;
            }

            // a declaration, but an argument that never got a type - a string literal, an unbound
            // `null`. coerced anyway, which is precisely what happened before any of this deferral
            // existed, so the diagnostic the type checker gives it is unchanged
            resolver.coerce_arguments(*call, mod->nodes);
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

        // fixpoint: each round takes every call as far as the types now known allow, and cloning a
        // template exposes its body's calls - with concrete types - for the next round
        //
        // the four steps are ordered, and the order is the design. see each one
        bool progressed = true;
        size_t rounds = 0;
        while (progressed) {
            progressed = false;

            if (++rounds > MAX_ROUNDS) {
                // the fixpoint did not converge. locate the report at any generic call still
                // unresolved so the user has a concrete site to look at rather than a silent stall
                for (auto &[call, mod] : snapshot_calls()) {
                    if (call->decl && call->decl->is_generic() && !_processed.count(call)) {
                        _collector.collect_issue<Issue::GenericError>(
                            code_ref_for(*mod, call->token_function_name),
                            "monomorphization did not converge after " + std::to_string(MAX_ROUNDS)
                                + " rounds: generic '" + call->decl->func_name() + "' could not be resolved");
                        break;
                    }
                }
                break;
            }

            progressed |= instantiate_generic_calls(rounds);

            // operand syntax whose meaning the types just settled: a bracket over a container, an
            // operator over what was a bare type parameter. **ahead of the re-derivation below**,
            // because a declaration inferred from `$a[0]` has no type at all until the element call
            // is attached - and behind the instantiation above, because it needs that round's types
            progressed |= _operators.run_round();

        // **after the rewriter, before the re-derivation.** after, because `foreach ($grid[0] as $row)`
        // has no source type until the bracket has become an `operator []` call - the very reason the
        // rewriter is itself ahead of the sweep. before, because `$__it` is declared with no type node
        // and that sweep is what types it, so lowering here saves a whole round.
        //
        // and before the ownership pass below, which walks a body **exactly once, ever**: the round a
        // loop lowers in has to be the round its body becomes eligible. OwnershipPass::body_is_concrete
        // answers false while an unlowered foreach is in the body, which is what makes that safe rather
        // than merely fast
        progressed |= _foreach.run_round();

            progressed |= rederive_stale_variable_types();
            progressed |= rederive_stale_capture_types();
            progressed |= settle_calls();

            // resolve ownership for every body that is concrete now: erase its `mv` markers, report
            // the copies it cannot make, and insert its drops. inside the loop for the same reason
            // the re-typing above is - it both *needs* the concrete types this round produced and
            // *produces* new generic call sites (a drop of a `Box<int32>` local calls Box<T>'s
            // destructor), which the next round instantiates through the ordinary path
            //
            // last of the four, so every call in a body it walks has already been fitted to its
            // parameters. a copy this pass inserts wraps an argument, and so does a coercion; doing
            // them in the other order would nest them the other way round
            progressed |= _ownership.run_round();
        }

        // **the fixpoint converged, so every transient node it owns has to be gone.** the two
        // rewriters leave a node in place while its types are still arriving, and nothing else ever
        // turns a permanent "not yet" into a diagnostic - PointerAdjuster throws for a survivor, and
        // it runs *before* run_semantic_passes gates on has_critical_issues(), so the abort landed on
        // top of whatever real error had explained it.
        //
        // in the round order, and for the round order's reason: a literal refused here leaves its
        // declaration untyped, and a loop over that declaration is what the next line refuses
        _operators.finalize();
        _foreach.finalize();

        finalize_calls();
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
        // concrete instantiations (Box<int32>) the user cares about
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
};
