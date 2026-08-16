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
#include "AST/ASTLiteralTyping.h"
#include "AST/ASTOwnership.h"
#include "AST/ASTPlaceExpr.h"
#include "Debugging.h"

#include <fmt/core.h>

#include <algorithm>
#include <ranges>
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
        : _bundle(bundle), _collector(bundle.collector), _ownership(bundle),
          _const_folding(bundle), _operators(bundle), _guards(bundle), _matches(bundle), _foreach(bundle),
          _interpolation(bundle)
    {
        _trace = std::getenv("ECO_TRACE_MONO") != nullptr;
    }

    CodeRef Monomorphizer::code_ref_for(Module &mod, const TokenReference &token)
    {
        return CodeRef{&mod, token.make_slice()};
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
    // implementation the parser also uses. a second copy of that loop would disagree about the
    // case B23 is: the parser runs for a concrete callee whose arguments are not typed yet, this
    // one only ever runs after substitution
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

                // **a type parameter the author *wrote* is not stale**, and it is the one shape here
                // that is not.
                //
                // undetermined covers two different things. `$b = Box<int32>(...)` captured the
                // template's un-substituted return type and `$x = f(...)` captured void - those are
                // missing information, and this sweep is what supplies it. A written `T` is not
                // missing anything: it is information that arrives at substitution, and deriving it
                // from the initializer instead **destroyed the declaration on the template**.
                // `T $sum = 0;` became `int32 $sum` there, so every instance cloned afterwards
                // declared an int32 - a `float64` one computing in int32 and an `int64` one
                // overflowing, both in silence. Invisible while every instance happened to be created
                // before this ran, which is why it only showed once a generic reached another generic
                // and was discovered a round later.
                //
                // **both halves are load-bearing.** `type_token` alone is too coarse - a written type
                // whose *name* did not resolve at parse time is undetermined for the first reason and
                // does want re-deriving, which is what `stream $out = std::io::stdout;` is. And
                // `contains_type_param` alone is too coarse the other way, since a captured
                // `Box<T>` mentions one and is exactly what this exists for. Written **and** a type
                // parameter is the intersection, and it is only ever the declaration in a template
                if (decl->type_node()->type_token.has_value() && contains_type_param(decl->type())) {
                    continue;
                }

                // **a guard's binding is not this sweep's to derive**, and the flag says exactly why:
                // `binds_unwrapped` *means* "this declaration's type is not its initializer's type", so
                // deriving it from the initializer is guaranteed to be one level wrong. a deferred one
                // carries an unknown placeholder that would look like an invitation - AST::GuardLowering
                // is what types it, from the payload the plan gave
                if (decl->binds_unwrapped) {
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
    // Capture is decided in the parser, at the read, so the property took the captured variable's type
    // as it stood *then*. For `$b = Box<int32>(5)` that was the template's un-substituted `Box<T>`,
    // stale for precisely the reason step B's variables are.
    //
    // Re-derived from the place the capture reads rather than from a remembered type. `captured_values[i]`
    // is what fills the property, so the two cannot drift, and it answers for a capture of any shape -
    // which the declaration behind it would not, a transitive capture reading the enclosing
    // environment's property rather than a local.
    //
    // Immediately after step B, so a variable that round made concrete retypes its capture in the same
    // round. And before step C, so the body's `$__env->b->get()` resolves against `Box<int32>` rather
    // than finding only the template's method and reaching codegen unresolved.
    //
    // One environment per closure *expression*, which a clone shares (ClosureExprNode::clone). That is
    // fine only while a closure cannot be written in a generic body - two instances' captures would
    // otherwise want two layouts and retype each other every round. The restriction lifts together with
    // the per-instantiation environment a generic closure would need
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
        // **a shorthand first, because `lookup_namespace` no longer tells the kinds apart on its own.**
        // a `.timeout(30)` has none either, so the member arm below would read its first *argument* as
        // a receiver and report that `int32` has no member `timeout` - a sentence about a type nobody
        // wrote, pointing away from the thing that is actually missing
        if (call.is_shorthand_static_call()) {
            // an owner was named and the type simply declares no such static. the owner is concrete
            // here, so this is a real answer rather than a round that ran out
            if (call.static_owner.has_complex_type()) {
                _collector.collect_issue<Issue::UnknownStaticFunction>(
                    at, call.token_function_name.value(), call.static_owner.get_type_desciption());
                return;
            }

            // nothing ever named an owner. reported at the `.`, which is where the missing information
            // belongs - the name is not the mistake
            _collector.collect_issue<Issue::UnboundShorthandCall>(
                at_token(at, call.token_shorthand_dot), call.token_function_name.value());
            return;
        }

        // which of the two remaining errors it is, is which kind of call it is - and that is
        // `lookup_namespace`, the one field that distinguishes them: a member call has none, because
        // its candidates come from the receiver sitting in argument 0
        if (call.lookup_namespace == nullptr
            && !call.arguments.empty() && call.arguments[0] != nullptr) {

            const ValueType receiver = target_type_of(call.arguments[0]->result_type());

            // **a receiver that still mentions a type parameter is a template's, not a program's.**
            // this call sits in an un-instantiated body, and the clones the fixpoint made are what
            // carry the concrete receiver - so `$c->count()` inside `f<T>(T& $c)` is answered by
            // `f<array<int32>>`'s copy and reporting the template's would blame the one body that
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

        // **shorthands last, and the order is the diagnostic.** the arena holds a call's arguments
        // ahead of the call itself, so `draw(.norm(2.0))` would reach the shorthand first and report
        // that nothing named its owner - true, but the thing worth saying is that `draw` is overloaded
        // and a shorthand cannot be what tells the overloads apart. letting the enclosing call speak
        // first lets it say so *and* mark the shorthand terminal, which is what leaves one diagnostic
        // rather than two for one mistake
        //
        // a shorthand nothing encloses is unaffected: its turn comes either way
        auto ordered = snapshot_calls();

        std::stable_partition(ordered.begin(), ordered.end(), [](const auto &entry) {
            return !entry.first->is_shorthand_static_call();
        });

        for (auto &[call, mod] : ordered) {
            // t_failed among the terminal states is what keeps this sweep from reporting a second
            // time on a call some round already reported. relying on the collector to
            // de-duplicate the identical message is not enough
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

            // **an argument that still mentions a type parameter is a template's, not a program's.**
            //
            // The same silence report_unknown_name keeps for a receiver, and for the same reason one
            // argument further out. This call sits in an un-instantiated body, the clones the fixpoint
            // made are what carry concrete argument types, and reporting the template's would blame the
            // one body that is never emitted.
            //
            // If nobody instantiated it there is nothing to report. If somebody did, the clone reports
            // for itself, with the types the author can actually see.
            //
            // What makes this load-bearing rather than tidy is an **overload set** over a type
            // parameter. `hash::of($key)` inside `map<K, V>` is undecidable in the template - every
            // concrete overload scores neutrally against a bare `K`, so match_function answers
            // t_undecidable - and it becomes decidable in `map<string, int32>`'s clone.
            //
            // Without this, a standard library type could not call an overload set on its own key type
            // at all, and the diagnostic named every overload in the set as though the author had
            // written an ambiguous call
            const bool argument_mentions_a_type_param = std::ranges::any_of(
                call->arguments,
                [](const ExprNode *argument) {
                    return argument != nullptr && contains_type_param(argument->result_type());
                });

            if (argument_mentions_a_type_param) {
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

            // **everything the language asked the compiler to decide, decided before anything else looks
            // at it.** three constraints pin this position and all three are load-bearing.
            //
            // *after instantiate_generic_calls*, because that is what binds `decl->instantiation_args[0]`
            // on `mem::is_trivially_copyable<T>()`. AST::classify_copy and AST::needs_destruction both
            // answer "no" for an unsettled type parameter on purpose - a not-yet rather than a refusal -
            // so folding either against a `T` the monomorphizer has not bound yet is silently the wrong
            // answer in the one direction that compiles. the same sentence ExprCodegen has always carried.
            //
            // *ahead of both rewriters below*, and this is the point rather than tidiness. They do not
            // merely waste work on a subtree about to vanish, they **create call sites in it**.
            //
            // An `array<T>`'s untaken copy arm is `$this[] = $other[$i]` - two bare IndexExprNodes at
            // clone time - and AST::OperatorRewriter is what turns them into `operator []` calls that the
            // next round instantiates and codegen emits. A `foreach` in an untaken arm is the same story
            // one level up: AST::ForeachLowering mints its `iterate()`. Discarding first is the only way
            // not to pay for either.
            //
            // *before the ownership pass*, which walks a body **exactly once, ever**. A `T $doomed` in an
            // untaken arm is a local that pass gives a drop, and a drop of an owning `T` is one more
            // generic call site. OwnershipPass::body_is_concrete answers false while a ConstIfNode or a
            // ConstExprNode is in the body, which is what makes this safe rather than merely early
            progressed |= _const_folding.run_round();

            // operand syntax whose meaning the types just settled: a bracket over a container, an
            // operator over what was a bare type parameter. **ahead of the re-derivation below**,
            // because a declaration inferred from `$a[0]` has no type at all until the element call
            // is attached - and behind the instantiation above, because it needs that round's types
            progressed |= _operators.run_round();

            // **after the rewriter and ahead of the loop lowering.** after, for two reasons that are the
            // rewriter's own: it performs the weak upgrade that turns a substituted `weak<Node>` into a
            // `Node?` - so the plan's nullable arm answers it rather than looking for a conformance - and
            // `$v = guard $slots[$i] else {...}` has no subject type until the bracket has become an
            // `operator []` call. ahead of the loop lowering, so a `foreach` over a guard's binding sees a
            // typed binding in this same round.
            //
            // and before the ownership pass below for its exact reason: OwnershipPass::body_is_concrete
            // answers false while GuardNode::plan_decided is false, so the round a guard's plan lands in is
            // the round its body becomes eligible
            progressed |= _guards.run_round();

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

            // **before the interpolation lowering, and that is the whole of why it sits here.** a
            // `"{$s}"` inside a match arm is lowered in the first round that reaches it - that pass has
            // no pending state and no finalize - so a payload binding still untyped at that moment is a
            // `str::from` overload chosen against nothing at all. the loop lowering above is ahead of
            // interpolation for exactly this reason and this is the same reason again.
            //
            // it derives its own subject's type rather than waiting for the sweep below, which is what
            // makes running here possible - AST::MatchResolution::resolve says why, and a guard's
            // binding sets the precedent.
            //
            // and before the ownership pass, which walks a body exactly once ever:
            // OwnershipPass::body_is_concrete answers false while MatchExprNode::patterns_decided is
            // false, so the round a match resolves in is the round its body becomes eligible
            progressed |= _matches.run_round();

            // beside the loop lowering, and for two of its three reasons. **before settle_calls**,
            // because the `str::from` and `str::concat` calls it mints are exactly what that has to
            // finish - one of them may name a user's own overload, or a generic the next round still
            // has to instantiate. **before the ownership pass**, because every one of those calls
            // returns an owning `string` and a body is walked exactly once, ever;
            // OwnershipPass::body_is_concrete answers false while an unlowered literal is in it.
            //
            // it needs nothing from the rewriter above, unlike the loop lowering: which overload
            // `str::from` picks is AST::CallResolver's question, asked whenever the hole settles, so
            // this pass never has to wait for a type and never has a pending state to finalize
            progressed |= _interpolation.run_round();

            progressed |= rederive_stale_variable_types();

            // **after the re-derivation, and that order is the content**: step B may be what makes a
            // declaration's type concrete in this very round, and a literal written at a type that is
            // still undetermined would simply be skipped. AST::type_destination_literals is the
            // live-tree walk; this is only the moment
            progressed |= type_destination_literals(_bundle);

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
        // first of the three, in the round order and for the round order's reason: a `const if` refused
        // here leaves an empty scope behind, and there is nothing left inside it for the two below to
        // blame - where the other way round they would report against arms that were never going to be
        // compiled
        _const_folding.finalize();
        _operators.finalize();
        _guards.finalize();
        _matches.finalize();
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
