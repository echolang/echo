#include "AST/ASTCallResolution.h"

#include "AST/ASTVariadic.h"

#include "AST/ASTArgumentBind.h"
#include "AST/ASTArgumentFit.h"
#include "AST/ASTArrayLiteral.h"
#include "AST/ASTCFunction.h"
#include "AST/ASTCast.h"
#include "AST/ASTCollector.h"
#include "AST/ASTConstness.h"
#include "AST/ASTFunctionMatcher.h"
#include "AST/ASTInstantiation.h"
#include "AST/ASTLiteralTyping.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTModule.h"
#include "AST/ASTNullability.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTValueType.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeCastNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"

#include <fmt/core.h>

#include <algorithm>
#include <cassert>
#include <ranges>

namespace AST
{
    namespace
    {
        // **is any of these candidates a near miss?** - which for an operator is the question "does any
        // of them name a type the author actually wrote here". operators all share the root namespace,
        // so an overload set is the whole program's rather than this use site's: `operator +(A, A)`
        // beside `$p + $q` where `$q` is a `B` is worth showing, and the standard library's
        // `operator ==(const string&, const string&)` beside two structs is not
        //
        // compared on the named type, through the borrow and the const a parameter is declared with,
        // since ValueType equality is exact and a `const A&` parameter would otherwise not match an `A`
        // operand. a primitive operand answers false and rightly: `int32` is in half the signatures a
        // program links
        bool candidates_mention_operands(
            const std::vector<FunctionDeclNode *> &candidates,
            const std::vector<ValueType> &operand_types)
        {
            const auto names_the_same_type = [](const ValueType &a, const ValueType &b) {
                return a.has_complex_type() && b.has_complex_type()
                    && a.get_complex_type() == b.get_complex_type();
            };

            for (const FunctionDeclNode *candidate : candidates) {
                for (const VarDeclNode *param : candidate->args) {
                    if (param == nullptr || !param->has_type()) {
                        continue;
                    }

                    const ValueType declared = value_type_of(param->type());

                    for (const ValueType &operand : operand_types) {
                        if (names_the_same_type(declared, value_type_of(operand))) {
                            return true;
                        }
                    }
                }
            }

            return false;
        }

        // when a parameter is a borrow and the argument is addressable, wrap it in an AddrOfExprNode
        // so its address is passed instead of a loaded value. this is the implicit form of the
        // address-of that `&$x` makes explicit
        //
        // The whole rule lives in argument_fit - which parameters auto-borrow, which arguments can be
        // borrowed, and that an argument which already fits is left alone. It has to, because overload
        // resolution predicts this decision exactly, and a candidate accepted there and then not
        // wrapped here would reach codegen passing a value where an address is expected.
        //
        // the wrap a borrow rank scored. t_borrow_through peels first - AddrOf(Deref) - because a
        // bare AddrOf of a `ptr<T>` would build `ptr<ptr<T>>`. the other four take the address of the
        // argument as it is. both land as AddrOf, which is what PointerAdjuster's argument arm already
        // routes through adjust_place without asking
        //
        // the difference among the four - whether the operand already has storage or has to be given
        // some - is a question about the operand's shape, which AST::OwnershipPass asks of the tree.
        ExprNode *borrow_if_wanted(NodeCollection &nodes, ExprNode *arg, ArgumentFit fit)
        {
            if (fit_is_borrow_through(fit)) {
                return borrow_through_pointer(nodes, arg);
            }

            // the four ranks that plant a bare AddrOf, asked of AST::borrow_place_if_wanted rather
            // than enumerated here: the const it gains is already on the operand's type, and which
            // ranks are those borrows is the fit ordering's own question. OwnershipPass plants the
            // same node at a T& declaration
            return borrow_place_if_wanted(nodes, arg, fit);
        }

        // a value handed to a parameter its own type declared a conversion to becomes a call to that
        // `#[implicit]` method. beside borrow_if_wanted because it is the same shape - ask the one fit
        // rule whether this case applies, and if so wrap the argument - and because this is the single
        // place that coerces arguments, so neither can be forgotten at some other call site.
        //
        // `at` locates the resulting call at the *caller*, not at the stdlib declaration, so anything
        // reported inside it points where the user wrote something
        ExprNode *convert_if_wanted(
            NodeCollection &nodes,
            ExprNode *arg,
            ArgumentFit fit,
            const ValueType &expected,
            const TokenReference &at
        )
        {
            if (fit != ArgumentFit::t_declared_conversion) {
                return arg;
            }

            // read once: MemberAccessNode::result_type() recurses the whole `->` chain, and the
            // receiver rule below asks the same question
            const ValueType arg_type = arg->result_type();

            // the same expression argument_fit ranked with, and through the same function, so this is
            // retrieval. a borrow parameter is answered by a conversion to its pointee, a borrowed
            // argument by the conversions its pointee declares, and the borrow of the conversion's
            // result is the separate rank the re-ask below picks up
            FunctionDeclNode *conversion = implicit_conversion_for(arg_type, arg, expected);

            // the rank identifies the case, so this is retrieval and not a second decision. sharing
            // t_conversion with the primitive casts one step below would make a null answer here
            // the only way to tell them apart
            assert(conversion != nullptr && "the fit rank promised a declared conversion");

            // the mint is AST::emit_declared_conversion's, shared with a written `$x as T`
            return emit_declared_conversion(nodes, arg, conversion, expected, at);
        }

        // the per-argument wrap: fit, a declared conversion, re-fit, then borrow. the residual
        // TypeCastNode is a separate step so a receiver refused for const-ness can skip it
        void wrap_argument(
            ExprNode *&slot,
            const ValueType &expected,
            const TokenReference &at,
            NodeCollection &nodes)
        {
            if (slot == nullptr || expression_produces_no_value(*slot)) {
                return;
            }

            ArgumentFit fit = argument_fit(slot->result_type(), slot, expected);
            ExprNode *converted = convert_if_wanted(nodes, slot, fit, expected, at);

            if (converted != slot) {
                fit = argument_fit(converted->result_type(), converted, expected);
            }

            slot = borrow_if_wanted(nodes, converted, fit);
        }

        void residual_cast(ExprNode *&slot, const ValueType &expected, NodeCollection &nodes)
        {
            if (slot == nullptr || expression_produces_no_value(*slot)) {
                return;
            }

            const ValueType coerced = slot->result_type();

            if (!is_implicitly_convertible(coerced, expected)
                || arrival_wraps_optional(coerced, expected)) {
                slot = &nodes.emplace_back<TypeCastNode>(expected, slot, true);
            }
        }

        void coerce_argument(
            ExprNode *&slot,
            const ValueType &expected,
            const TokenReference &at,
            NodeCollection &nodes)
        {
            wrap_argument(slot, expected, at, nodes);
            residual_cast(slot, expected, nodes);
        }

        // **a written `null` argument takes the parameter's type**, which is the one thing about an
        // argument that has to be decided here rather than in the parser.
        //
        // Every other position that admits a null hands the destination down to parse_expr, and the
        // null arm binds it there. A *direct* call cannot. Its parameter types are on a declaration
        // nobody has chosen yet, so Parser::parse_call_arguments passes no expected type at all and the
        // null is parsed untyped. An indirect call reads them off the callee's signature and does bind,
        // which is why `$fn(null)` worked and `f(null)` did not.
        //
        // This is the first point in the pipeline holding both the argument node and a resolved
        // parameter, so it is where the binding belongs. Two things went wrong without it, and one call
        // fixes both. An unbound null reached codegen with no type, where a wrapped `T?` destination has
        // no null address to be and TypeLowering::coerce_value refused it. And before that, it stayed
        // permanently undetermined, so arguments_are_determined below could never let the call settle.
        //
        // A parameter that does *not* admit a null is left alone on purpose. AST::bind_null_to declines
        // it, the call stays pending, and AST::TypeChecker reports it against the destination through
        // AST::null_rejection_reason - which is the diagnostic that names `Foo?`.
        //
        // **an array literal is the second thing an argument position has to type**, for the same reason
        // spelled the same way. It has no type of its own, its destination is on a declaration nobody
        // had chosen at parse time, and this is the first point holding both. One loop, two rules - each
        // still its own function, so neither grew an arm about the other.
        //
        // The question asked is whether an argument is an array literal still waiting to be *expanded*,
        // which is not the same as being typed. AST::OperatorRewriter turns the literal into a
        // declaration plus one append per element and puts the declaration's name here, so coercing
        // against the literal would fit the wrong node - it is `t_addressless`, and a borrow parameter
        // would get a cast where an address belongs. So the call waits a round, exactly as an
        // undetermined argument does
        // **and a number literal is the fourth**, for the third time the same reason. `$a[] = 2.5` on
        // an `array<int32>` reaches its destination as an operand of a synthesized `operator []` call,
        // and the ordinary argument conversion narrowed it - correct for a variable, and silent data
        // loss for a literal, which is the one thing every *written* destination refuses. So the check
        // that refuses `int32 $x = 2.5;` is asked here too, at the first point holding both the
        // argument node and a resolved parameter.
        //
        // it also completes the other half of a bound type parameter: `can_instantiate` no longer lets
        // an untyped literal decide what `T` is, and this is what then types it *at* whatever the
        // concrete arguments decided. so `pick(0, $n)` over a `usize $n` binds `usize` and the `0` is
        // written at it, rather than binding `int32` and truncating `$n`
        template <typename ExpectedAt>
        bool bind_destination_typed_arguments(
            std::vector<ExprNode *> &arguments,
            ExpectedAt expected_at,
            size_t parameter_count,
            const CoreTypes &core, NodeCollection &nodes,
            Collector &collector, const CodeRef &at)
        {
            bool waiting_on_a_literal = false;

            for (size_t i = 0; i < arguments.size() && i < parameter_count; i++) {
                if (arguments[i] == nullptr) {
                    continue;
                }

                const ValueType expected = expected_at(i);

                bind_null_to(arguments[i], expected);
                bind_function_ref_to(arguments[i], expected, collector);

                // **the one destination a shorthand cannot reach at parse time**, which is why it is
                // here rather than only in the expression parser: a parameter's type sits on a
                // declaration nobody had chosen yet when the argument was read
                //
                // nothing is returned into `waiting_on_a_literal`: an unbound shorthand answers `void`
                // from result_type(), so arguments_are_determined below already holds the call for it
                bind_shorthand_to(arguments[i], expected);

                if (bind_array_literal_to(arguments[i], expected, core)) {
                    waiting_on_a_literal = true;
                }

                // asked only of a literal nobody has typed, so a round that runs again over a settled
                // argument does nothing - and so an *explicit* cast the author wrote is never undone
                if (is_untyped_literal(arguments[i])) {
                    const LiteralTyping typing =
                        type_literal_at(arguments[i], value_type_of(expected), nodes);
                    const CodeRef here = code_ref_at_literal(at, arguments[i]);

                    report_literal_warning(collector, here, typing);

                    if (typing.result == LiteralTyping::Result::t_refused) {
                        report_literal_refusal(collector, here, typing);
                    }
                    else {
                        arguments[i] = typing.node;
                    }
                }
            }

            return !waiting_on_a_literal;
        }

        // the first argument that is a shorthand nothing has named an owner for, or null. what a tie
        // needs to know before it words itself: an argument with no type of its own is why the
        // candidates could not be told apart, and it is not one a cast can fix
        FunctionCallExprNode *first_unbound_shorthand_argument(const FunctionCallExprNode &call)
        {
            for (auto *arg : call.arguments) {
                if (auto *shorthand = unbound_shorthand_call_of(arg)) {
                    return shorthand;
                }
            }

            return nullptr;
        }

        // true when every argument's type is known, so a decision made about them is final rather
        // than premature
        bool arguments_are_determined(const std::vector<ExprNode *> &arguments)
        {
            for (auto *arg : arguments) {
                // a hole left by a failed parse cannot be waited on - there is nothing coming that
                // would give it a type, and the diagnostic for it was already reported where it was
                // read
                if (arg == nullptr) {
                    continue;
                }

                // **a variadic pack is answered by its elements**, not by itself. an array literal's
                // result_type() is unknown by design and stays unknown here - nothing expands a pack
                // into a declaration the way a collection literal is expanded - so asking the node
                // would leave every call to a C variadic function pending forever
                if (auto *pack = variadic_pack_of(arg)) {
                    for (const auto *element : pack->elements) {
                        if (element != nullptr && !type_is_determined_for_fit(element->result_type())) {
                            return false;
                        }
                    }

                    continue;
                }

                // AST::type_is_determined_for_fit: a match binding is declared `unknown&`, and
                // ranking that as determined lets a lone candidate settle by match rule 2
                if (!type_is_determined_for_fit(arg->result_type())) {
                    return false;
                }
            }

            return true;
        }

        // bind destination-typed arguments, wait until they are determined, then coerce. shared
        // by both settle overloads: an indirect call has no declaration, but the same last step
        //
        // after the generic gate, so a `T?` parameter is never what a null learns its shape from -
        // the round that rewires `decl` to the instance is the first one with a concrete type to
        // bind. an array literal also makes the call wait: what finally reaches the parameter is
        // the declaration AST::OperatorRewriter hoists, not the literal itself
        //
        // coercing against a type that says nothing cannot tell "no conversion needed" from "no
        // information": the borrow rule declines to wrap, and the residual cast fires for a
        // mismatch that was never a mismatch. so wait instead
        template <typename ExpectedAt, typename Coerce>
        CallResolver::Result fit_arguments(
            std::vector<ExprNode *> &arguments,
            ExpectedAt expected_at,
            size_t parameter_count,
            Collector &collector,
            NodeCollection &nodes,
            const CodeRef &at,
            Coerce coerce
        )
        {
            if (!bind_destination_typed_arguments(
                    arguments, expected_at, parameter_count,
                    collector.core_types, nodes, collector, at)) {
                return CallResolver::Result::t_pending;
            }

            if (!arguments_are_determined(arguments)) {
                return CallResolver::Result::t_pending;
            }

            coerce();
            return CallResolver::Result::t_settled;
        }
    }

    std::vector<FunctionDeclNode *> CallResolver::candidates_for(const FunctionCallExprNode &call) const
    {
        // **constructing a type**: `T(...)` after the callee is a type rather than a function name.
        // asked first so a constructor's arguments cannot be read as a receiver or a free call - a
        // type parameter is a not-yet (empty → t_unknown_name, retryable), a primitive or interface
        // is a real empty set. a struct or class looks up the same overload set a written `Foo(...)`
        // already uses: constructors are namespace functions named after the type
        if (!call.constructed_type.is_unknown()) {
            if (is_undetermined_type(call.constructed_type) || !call.constructed_type.has_complex_type()) {
                return {};
            }

            const ComplexType *owner = call.constructed_type.get_complex_type()->template_or_self();

            if (!owner->name.has_value() || owner->ast_namespace == nullptr) {
                return {};
            }

            return _collector.functions.overloads(*owner->name, *owner->ast_namespace);
        }

        // a **static** call: the type names the overload set. a closed search - falling through
        // would let argument 0 be read as a receiver, so `usize::nope($s)` would become `$s->nope()`.
        // asked ahead of the namespace arm because `Type::f()` carries both a written owner and the
        // namespace the parser was standing in, and the owner is the one that decides
        //
        // a type-parameter owner is a not-yet, the same empty set a shorthand has: `T::from(...)`
        // inside a template body has nothing to search until substitution names the type
        if (!call.static_owner.is_unknown()) {
            return find_static_functions(
                _collector,
                call.static_owner,
                call.lookup_name(),
                call.token_function_name
            );
        }

        // a shorthand whose destination has not named an owner yet. empty rather than falling through
        // to either arm below: `.ok(5)`'s arguments are not a receiver, and reading argument 0 as one
        // is what reported "int32 has no member ok". CallResolver::settle turns an empty set into the
        // retryable t_unknown_name, which is exactly the not-yet this is
        if (call.is_shorthand_static_call()) {
            return {};
        }

        // a free call: the namespace it was written in, searched outward by the registry
        if (call.lookup_namespace != nullptr) {
            return _collector.functions.overloads(call.lookup_name(), *call.lookup_namespace);
        }

        // a member call. the receiver is argument 0, already addressed by the parser as
        // `AddrOf(Deref^n(recv))`, so the type to look on is what `->` reaches through - every
        // pointer level, which is AST::target_type_of
        if (call.arguments.empty() || call.arguments[0] == nullptr) {
            return {};
        }

        const ValueType receiver_type = target_type_of(call.arguments[0]->result_type());
        if (!receiver_type.has_complex_type()) {
            return {};
        }

        return find_member_functions(receiver_type.get_complex_type(), call.lookup_name());
    }

    namespace
    {
    void apply_binding_to_call(
        FunctionCallExprNode &call,
        NodeCollection &nodes,
        TypeRegistry &registry
    )
    {
        if (call.decl == nullptr) {
            return;
        }

        if (call.argument_names.empty() && call.arguments.size() == call.decl->args.size()) {
            return;
        }

        const ArgumentBinding binding = bind_arguments(*call.decl, call.arguments, call.argument_names);
        assert(binding.kind == ArgumentBindKind::t_ok && "choose_declaration already bound this call");

        apply_argument_binding(call, binding, nodes, registry);
    }
    }

    CallResolver::Result CallResolver::choose_declaration(
        FunctionCallExprNode &call,
        const std::vector<FunctionDeclNode *> &candidates,
        const CodeRef &at,
        bool report
    )
    {
        const std::string &name = call.token_function_name.value();

        // pass 2 registers the memberwise constructor with a signature whose arity may still
        // shrink once `init` has been parsed. only types that declared `init` (the slot is filled
        // in pass 2, before any body) need this wait - every other implicit constructor's arity
        // is already final, and leaving those pending until after parse types `$a = Foo(...)`
        // as void for a round, which PointerAdjuster and CastResolution then mis-read
        if (at.module != nullptr && !at.module->construction_finalized) {
            for (auto *candidate : candidates) {
                if (candidate == nullptr
                    || !candidate->is_implicitly_generated
                    || !candidate->is_constructor()) {
                    continue;
                }

                if (find_init(enclosing_type_of(*candidate)) != nullptr) {
                    return Result::t_pending;
                }
            }
        }

        struct BoundCandidate
        {
            FunctionDeclNode *decl = nullptr;
            ArgumentBinding binding;
        };

        std::vector<BoundCandidate> bound;
        bound.reserve(candidates.size());

        for (auto *candidate : candidates) {
            ArgumentBinding binding = bind_arguments(
                *candidate, call.arguments, call.argument_names);

            if (binding.kind != ArgumentBindKind::t_ok) {
                continue;
            }

            bound.push_back(BoundCandidate { candidate, std::move(binding) });
        }

        if (bound.empty()) {
            // bind failures are final, like t_no_viable: a name that matches nothing, a missing
            // label, an unfilled required parameter. they are not waiting on a later type.
            // construction calls are not resolved at parse, so this is never asked against a
            // memberwise parameter list that finalize_module_construction still rewrites
            report_argument_bind_failure(_collector, call, candidates, at);
            return Result::t_failed;
        }

        const std::vector<ValueType> argument_types = argument_types_of(call);

        // a non-operator lone bind survivor is a unique name even when other overloads were in
        // the set; an operator set is still filtered because it is the whole program
        const bool operator_set = candidates.size() > 1 && candidates.front()->is_operator();
        const bool lone_bind_survivor = bound.size() == 1;
        const bool filter_instantiation = operator_set || !lone_bind_survivor;

        std::vector<FunctionCandidate> match_candidates;
        match_candidates.reserve(bound.size());

        for (BoundCandidate &entry : bound) {
            auto *candidate = entry.decl;
            auto parameter_types = candidate->parameter_types();
            const BoundSlots slots = bound_slots(entry.binding);

            if (filter_instantiation && candidate->is_generic()) {
                // score a template against the parameters it would actually be instantiated with,
                // not against the bare `T`. an unsubstituted parameter is undetermined, which the
                // matcher treats as neutral - so `pick<T>(T)` would tie with `pick(int32)` for a
                // float64 argument and lose the non-generic tiebreak, calling the concrete overload
                // through a narrowing conversion when the template matched exactly
                //
                // the same question the monomorphizer asks of the call it commits to, asked here of
                // a candidate that may be discarded - so only `fit` is read. the blame fields are
                // deliberately ignored: a constraint that rejects a template filters it out of the
                // set, and reporting it here would turn an overload the user never meant into an error
                //
                // **the owner goes in here too, not only at the monomorphizer's ask.** a static
                // overload set over a generic owner is scored against these substituted parameters,
                // and without the seed every candidate is still holding a bare `T` - so they all
                // rank undetermined and tie, and the call never resolves
                const ValueType &owner = call.static_owner.is_unknown() ? call.constructed_type : call.static_owner;
                const Instantiation inst = can_instantiate(
                    candidate,
                    slots.types,
                    _collector.type_registry,
                    explicit_type_args_of(call),
                    owner,
                    slots.defers);

                // the template cannot be instantiated for these arguments at all, so it is not a
                // candidate. this is also how a type constraint filters an overload set
                if (inst.fit == InstantiationFit::t_no) {
                    continue;
                }

                // t_maybe leaves the parameters as written, still mentioning `T`, which the matcher
                // reads as undetermined - the honest answer while the call sits in a template body
                // whose own parameters are not bound yet
                if (inst.fit == InstantiationFit::t_yes) {
                    for (auto &parameter_type : parameter_types) {
                        parameter_type = substitute_type(parameter_type, inst.bindings, _collector.type_registry);
                    }
                }
            }

            match_candidates.push_back(FunctionCandidate {
                .decl = candidate,
                .parameter_types = std::move(parameter_types),
                .is_generic = candidate->is_generic(),
                .argument_types = slots.types,
                .arguments = slots.exprs,
            });
        }

        const auto match = match_function(match_candidates);

        switch (match.outcome) {
        case FunctionMatch::Outcome::t_resolved:
            call.decl = match.decl;
            call.settlement = CallSettlement::t_uncoerced;
            return Result::t_settled;

        case FunctionMatch::Outcome::t_undecidable:
            // several candidates fit and the arguments that would separate them have no type yet - an
            // unbound `null`, a string literal, a variable typed from a generic call. the only
            // deferrable outcome: the fixpoint may answer those types, and reporting here would
            // reject a program that is perfectly well typed. `decl` stays null, which
            // result_type() answers as unknown and is_undetermined_type reads as "no information", so
            // a caller waiting on *this* call is undecidable in turn rather than wrongly decided
            if (!report) {
                return Result::t_pending;
            }

            // **an unbound shorthand argument is a different sentence**, and the remedy is why: the
            // message below tells the reader to cast the argument, and a `.f(...)` has no type to cast
            // *from*. the same shape as the t_no_viable arm below asking is_written_null before it
            // words its own refusal
            if (auto *shorthand = first_unbound_shorthand_argument(call)) {
                _collector.collect_issue<Issue::AmbiguousShorthandCall>(
                    at_token(at, shorthand->token_shorthand_dot),
                    fmt::format(
                        "The overload of '{}' cannot be chosen: '.{}(...)' has no type of its own, so "
                        "nothing here separates these:{}",
                        name, shorthand->token_function_name.value(), describe_candidates(match.tied)));

                // **the shorthand is finished too, and saying so is what keeps this one diagnostic.**
                // it is still unresolved, so the monomorphizer's finalizing sweep would reach it and
                // report that nothing named its owner - true, and already the whole content of the
                // sentence above. `t_failed` is terminal, which is exactly the "some round already
                // reported this" that sweep skips on
                shorthand->settlement = CallSettlement::t_failed;

                return Result::t_failed;
            }

            _collector.collect_issue<Issue::AmbiguousCall>(at, fmt::format(
                "The call to '{}' cannot be resolved: the types of its arguments are not known "
                "here, and these overloads all remain possible:{}\nAn explicit cast on the "
                "argument picks one.",
                name, describe_candidates(match.tied)));
            return Result::t_failed;

        case FunctionMatch::Outcome::t_ambiguous:
            // final the first time it is seen, whoever is asking: the matcher routes every tie that
            // an undetermined argument had a hand in to t_undecidable above, so a tie reaching here
            // was decided on types that are already known and no later round can break it
            _collector.collect_issue<Issue::AmbiguousCall>(at, fmt::format(
                "The call to '{}' is ambiguous. These overloads all match equally well:{}",
                name, describe_candidates(match.tied)));
            return Result::t_failed;

        case FunctionMatch::Outcome::t_no_viable:
        case FunctionMatch::Outcome::t_no_candidates:
            // also final, and for the mirror reason: argument_fit answers t_undetermined and never
            // t_none for an argument with no type, so nothing viable was rejected for being unknown
            //
            // t_no_candidates here means generic instantiation filtered every candidate out, so
            // nothing reached the matcher for it to have tied - the declarations that were tried are
            // what the user needs to see either way
            //
            // **an operator with nothing to list says it in its own words.** the third place that rule
            // is applied, for the same reason as the two in AST::TypeChecker: every `operator`
            // declaration in the program shares the root namespace, so a program carries every
            // operator's overload set whether it uses those types or not.
            //
            // `$s[] = 2;` on a slice was answered with nine candidates naming `map<K,V>` and
            // `ordered_map<K,V>`, and `$p == null` on a struct with a list naming `const string&`.
            //
            // **gated on `match.tied` being empty**, and that gate is the whole of the distinction. A tie
            // is made of candidates that nearly matched, so those *are* worth naming - indexing an
            // `array<int32>` with a string is best answered by showing that the parameter is a `usize`.
            // An empty tie is the fallback below reaching for every declaration in the root namespace,
            // and that list is the same in every program whatever it was written about.
            //
            // match rule 2 takes a lone candidate without consulting types at all, so a one-declaration
            // set would reach the type checker's wording by accident. a second `==` pair makes it a
            // real choice, and the message has to stay an operator refusal rather than degrade with it
            if (!candidates.empty() && candidates.front()->is_operator()) {
                const std::string spelling = candidates.front()->operator_spelling();
                const Operator *op = _collector.operators.get_operator(spelling);

                // **what is wrong with the operands comes first**, and it is the same rule
                // TypeChecker::visitBinaryExpr reads for a use site the parser kept as a
                // BinaryExprNode. which of the two a program reaches is decided by whether *anybody*
                // declared an infix form of the symbol, so the answer must not depend on it
                //
                // asked here rather than ahead of the matcher because it is a fallback: a declared
                // `operator (P $a) == (P? $b)` makes `$p == null` resolve, and a pre-gate would refuse
                // a call that had a perfectly good candidate. operands are `parse_time_operand`,
                // AST::PointerAdjuster running long after the fixpoint this sits in
                if (op != nullptr && call.arguments.size() == 2) {
                    const auto refusal = binary_operand_refusal(op,
                        parse_time_operand(call.arguments[0]),
                        parse_time_operand(call.arguments[1]));

                    if (refusal.has_value()) {
                        _collector.collect_issue<Issue::NoMatchingOverload>(at, *refusal);
                        return Result::t_failed;
                    }
                }

                // **a written `null` is its own refusal, whatever the tie says.** an unbound null has no
                // type, so argument_fit answers t_undetermined for it against every candidate - the tie
                // it produces is an artifact of the operand nobody could rank rather than a set of near
                // misses, and the list would be the whole root namespace either way. the wording is
                // AST::null_operand_refusal's, shared with the type checker, which reaches this same
                // refusal from the other direction: one overload, taken by the matcher without
                // consulting types at all, and refused by the coercion afterwards
                const bool has_null_operand = std::any_of(
                    call.arguments.begin(), call.arguments.end(),
                    [](const ExprNode *operand) { return is_written_null(operand); });

                if (has_null_operand) {
                    _collector.collect_issue<Issue::NoMatchingOverload>(at, null_operand_refusal(spelling));
                    return Result::t_failed;
                }

                // **a candidate that names neither operand is somebody else's declaration.** every
                // `operator` shares the root namespace, so a program carries every operator's overload
                // set whether it uses those types or not - and `$a == $b` on a struct was answered by
                // listing the standard library's `string` pair, a type no file of the author's
                // mentions. where nothing in the set is a near miss the useful sentence is the one a
                // use site had before any operator was declared anywhere: these operands have no
                // meaning for this symbol
                //
                // the converse is the whole reason this is a question rather than "is it built-in":
                // `operator +(A, A)` beside `$p + $q` where `$q` is a `B` *is* a near miss, and the
                // candidate list is exactly what says so. so is every custom symbol, whose candidates
                // are by definition the author's
                if (op != nullptr && !op->is_custom() && call.arguments.size() == 2
                    && !candidates_mention_operands(candidates, argument_types)) {
                    _collector.collect_issue<Issue::NoMatchingOverload>(at,
                        binary_unsupported_operands(op,
                            parse_time_operand(call.arguments[0]),
                            parse_time_operand(call.arguments[1])));
                    return Result::t_failed;
                }

                if (match.tied.empty()) {
                    _collector.collect_issue<Issue::NoMatchingOverload>(at, fmt::format(
                        "no overload of operator '{}' accepts {}. Declare one for it, or convert the "
                        "operands first.",
                        spelling, describe_operands(argument_types)));
                    return Result::t_failed;
                }
            }

            _collector.collect_issue<Issue::NoMatchingOverload>(at, fmt::format(
                "No overload of '{}' accepts these arguments. Candidates are:{}",
                name, describe_candidates(match.tied.empty() ? candidates : match.tied)));
            return Result::t_failed;
        }

        return Result::t_failed;
    }

    void CallResolver::coerce_arguments(FunctionCallExprNode &call, NodeCollection &nodes)
    {
        assert(call.decl != nullptr && "coercing a call that has no declaration");

        for (size_t i = 0; i < call.arguments.size() && i < call.decl->args.size(); i++) {
            if (call.arguments[i] == nullptr || expression_produces_no_value(*call.arguments[i])) {
                continue;
            }

            const ValueType expected = call.decl->args[i]->type();
            ExprNode *argument = call.arguments[i];

            // **a variadic tail is coerced element by element, and to nothing the declaration said.**
            // there is no parameter on the other side of one, so what each element is coerced to is
            // C's own answer for an argument that has none - AST::variadic_promotion_of. done here
            // because this is the only thing that coerces arguments, which is what keeps codegen from
            // carrying a second, differing copy of the promotion table
            // and a tail position that did *not* receive a list is left exactly as written. there is
            // no conversion to a `variadic_args` and never will be - AST::TypeChecker reports the
            // shape, and a cast minted here would bury that under "cannot implicitly convert"
            if (is_variadic_args(expected, _collector.core_types)
                && array_literal_of(argument) == nullptr) {
                continue;
            }

            if (auto *pack = variadic_pack_of(argument)) {
                for (auto *&element : pack->elements) {
                    if (element == nullptr) {
                        continue;
                    }

                    const ValueType from = element->result_type();
                    const ValueType promoted = variadic_promotion_of(from);

                    if (!(from == promoted)) {
                        element = &nodes.emplace_back<TypeCastNode>(promoted, element, true);
                    }
                }

                continue;
            }

            wrap_argument(call.arguments[i], expected, call.token_function_name, nodes);

            // **a receiver refused for its const-ness gets no residual cast.** `ptr<const Foo>` and
            // `ptr<Foo>` are the same value, so there is nothing here for codegen to lower - the
            // cast's only effect would be visitTypeCast reporting "cannot implicitly convert",
            // drowning the located refusal AST::TypeChecker::check_receiver_const words about the
            // same call
            if (i == 0 && const_receiver_refused(*call.decl, call.arguments[i]->result_type())) {
                continue;
            }

            residual_cast(call.arguments[i], expected, nodes);
        }

        call.settlement = CallSettlement::t_settled;
    }

    void CallResolver::coerce_arguments(IndirectCallExprNode &call, NodeCollection &nodes)
    {
        const ValueType callee_type = call.callee_type();
        assert(callee_type.has_signature() && "coercing an indirect call with no signature");

        const auto &signature = callee_type.signature();

        for (size_t i = 0; i < call.arguments.size() && i < signature.parameter_types.size(); i++) {
            coerce_argument(
                call.arguments[i], signature.parameter_types[i], call.token, nodes);
        }

        call.settlement = CallSettlement::t_settled;
    }

    CallResolver::Result CallResolver::settle(
        FunctionCallExprNode &call,
        NodeCollection &nodes,
        const CodeRef &at,
        bool report
    )
    {
        // both terminal states answer from the node, so asking again costs nothing and reports
        // nothing - which is what lets the fixpoint ask about every call every round
        if (call_is_terminal(call.settlement)) {
            return call.settlement == CallSettlement::t_settled ? Result::t_settled : Result::t_failed;
        }

        // the state, not `decl == nullptr`, decides which half runs. they agree for every call this
        // resolver made, and differ for the one other producer of a decided call - the ownership
        // pass, whose drops and copies name their callee outright and owe only the coercion
        if (call.settlement == CallSettlement::t_unresolved) {
            const auto candidates = candidates_for(call);

            // retryable, deliberately: a member call's candidates come from its receiver's type,
            // which a later round may still make concrete. so this is not a terminal state
            if (candidates.empty()) {
                // a leftover case is in the table and not in the overload set, so "no such function"
                // is the wrong sentence. final either way: the case list is complete before any body
                // is parsed, and a later round declares no leftover constructor
                if (!call.static_owner.is_unknown()) {
                    const std::string refusal =
                        enum_case_construction_refusal(call.static_owner, call.lookup_name());

                    if (!refusal.empty()) {
                        _collector.collect_issue<Issue::GenericError>(at, refusal);
                        call.settlement = CallSettlement::t_failed;
                        return Result::t_failed;
                    }
                }

                return Result::t_unknown_name;
            }

            const auto chosen = choose_declaration(call, candidates, at, report);

            if (chosen == Result::t_failed) {
                call.settlement = CallSettlement::t_failed;
            }

            if (chosen != Result::t_settled) {
                return chosen;
            }
        }

        // a generic callee is the monomorphizer's: it determines the type arguments, clones the
        // instance and rewires `decl` to it, and only then are there concrete parameters to fit
        // anything to. so this half of the state machine runs again after that one
        if (call.decl->is_generic()) {
            return Result::t_pending;
        }

        // after the instance exists, so a default still mentioning T is cloned from the substituted
        // recipe rather than the template's. names stay on the call until here, which is what lets
        // can_instantiate bind a named list against the template before this runs
        apply_binding_to_call(call, nodes, _collector.type_registry);

        return fit_arguments(
            call.arguments,
            [&](size_t i) { return call.decl->args[i]->type(); },
            call.decl->args.size(),
            _collector,
            nodes,
            at,
            [&] { coerce_arguments(call, nodes); }
        );
    }

    CallResolver::Result CallResolver::settle(
        IndirectCallExprNode &call,
        NodeCollection &nodes,
        const CodeRef &at
    )
    {
        if (call_is_terminal(call.settlement)) {
            return call.settlement == CallSettlement::t_settled ? Result::t_settled : Result::t_failed;
        }

        const ValueType callee_type = call.callee_type();

        if (is_undetermined_type(callee_type)) {
            return Result::t_pending;
        }

        // TypeChecker words a determined non-callable. terminal so OwnershipPass can walk
        if (!callee_type.has_signature()) {
            call.settlement = CallSettlement::t_failed;
            return Result::t_failed;
        }

        const auto &signature = callee_type.signature();

        return fit_arguments(
            call.arguments,
            [&](size_t i) { return signature.parameter_types[i]; },
            signature.parameter_types.size(),
            _collector,
            nodes,
            at,
            [&] { coerce_arguments(call, nodes); }
        );
    }

    bool arguments_mention_a_type_param(const std::vector<ExprNode *> &arguments)
    {
        // **an argument that still mentions a type parameter is a template's, not a program's.**
        //
        // this call sits in an un-instantiated body, the clones the fixpoint made are what carry
        // concrete argument types, and reporting the template's would blame the one body that is
        // never emitted. if nobody instantiated it there is nothing to report. if somebody did,
        // the clone reports for itself, with the types the author can actually see.
        //
        // load-bearing for an overload set over a type parameter. `hash::of($key)` inside
        // `map<K, V>` is undecidable in the template - every concrete overload scores neutrally
        // against a bare `K`, so match_function answers t_undecidable - and it becomes decidable
        // in `map<string, int32>`'s clone
        return std::ranges::any_of(arguments, [](const ExprNode *argument) {
            return argument != nullptr && contains_type_param(argument->result_type());
        });
    }
};
