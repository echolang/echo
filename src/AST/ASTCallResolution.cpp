#include "AST/ASTCallResolution.h"

#include "AST/ASTArgumentFit.h"
#include "AST/ASTCollector.h"
#include "AST/ASTFunctionMatcher.h"
#include "AST/ASTInstantiation.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTNullability.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeCastNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"

#include <fmt/core.h>

#include <cassert>

namespace AST
{
    namespace
    {
        // when a parameter is a borrow and the argument is addressable, wrap it in an AddrOfExprNode
        // so its address is passed instead of a loaded value. this is the implicit form of the
        // address-of that `&$x` makes explicit
        //
        // the whole rule - which parameters auto-borrow, which arguments can be borrowed, and that an
        // argument which already fits is left alone - lives in argument_fit, because overload
        // resolution has to predict this decision exactly. a candidate accepted there and then not
        // wrapped here would reach codegen passing a value where an address is expected
        //
        // **both borrow ranks produce the same node, and that is the point.** the difference between
        // them - whether the operand already has storage or has to be given some - is a question about
        // the operand's shape, which AST::OwnershipPass asks of the tree. nothing between here and
        // codegen should be able to tell the two apart, and AST::PointerAdjuster's argument arm already
        // routes any AddrOf through adjust_place without asking
        ExprNode *borrow_if_wanted(NodeCollection &nodes, ExprNode *arg, ArgumentFit fit)
        {
            if (fit != ArgumentFit::t_borrow && fit != ArgumentFit::t_borrow_temporary) {
                return arg;
            }

            return &nodes.emplace_back<AddrOfExprNode>(arg);
        }

        // a value handed to a parameter its own type declared a conversion to becomes a call to that
        // `#[implicit]` method. beside borrow_if_wanted because it is the same shape - ask the one fit
        // rule whether this case applies, and if so wrap the argument - and because this is the single
        // place that coerces arguments, so neither can be forgotten at some other call site.
        //
        // `at` locates the resulting call at the *caller*, not at the stdlib declaration, so anything
        // reported inside it points where the user wrote something
        ExprNode *convert_if_wanted(
            NodeCollection &nodes, ExprNode *arg, ArgumentFit fit, const ValueType &expected,
            const TokenReference &at)
        {
            if (fit != ArgumentFit::t_declared_conversion) {
                return arg;
            }

            FunctionDeclNode *conversion = find_implicit_conversion(arg->result_type(), expected);

            // the rank identifies the case, so this is retrieval and not a second decision - the two
            // used to share t_conversion with the primitive casts one step below, and a null answer
            // here was how this told them apart
            assert(conversion != nullptr && "the fit rank promised a declared conversion");

            // the receiver is addressed here, exactly as the parser addresses a method's and as
            // OwnershipPass::emit_resolved_member_call does: the conversion's `$this` is a borrow
            auto &conversion_call = nodes.emplace_back<FunctionCallExprNode>(
                at, std::vector<ExprNode *>{ &nodes.emplace_back<AddrOfExprNode>(arg) });

            // settled outright, unlike the ownership pass's calls: the callee is known *and* its one
            // argument is the address just built, which is exactly what its borrow parameter wants. so
            // there is nothing left for a later round to decide, and nothing that would make the
            // fixpoint revisit a call this deep inside an already-settled one
            conversion_call.decl = conversion;
            conversion_call.settlement = CallSettlement::t_settled;

            return &conversion_call;
        }

        // **a written `null` argument takes the parameter's type**, which is the one thing about an
        // argument that has to be decided here rather than in the parser.
        //
        // every other position that admits a null hands the destination down to parse_expr and the null
        // arm binds it there. a *direct* call cannot: its parameter types are on a declaration nobody has
        // chosen yet, so Parser::parse_call_arguments passes no expected type at all and the null is
        // parsed untyped. an indirect call reads them off the callee's signature and does bind, which is
        // why `$fn(null)` worked and `f(null)` did not
        //
        // this is the first point in the pipeline holding both the argument node and a resolved parameter,
        // so it is where the binding belongs. two things went wrong without it, and one call fixes both:
        // an unbound null reached codegen with no type, where a wrapped `T?` destination has no null
        // address to be and TypeLowering::coerce_value refused it - and, before that, it stayed
        // permanently undetermined, so arguments_are_determined below could never let the call settle
        //
        // a parameter that does *not* admit a null is left alone on purpose. AST::bind_null_to declines
        // it, the call stays pending, and AST::TypeChecker reports it against the destination through
        // AST::null_rejection_reason - which is the diagnostic that names `Foo?`
        void bind_null_arguments(FunctionCallExprNode &call)
        {
            for (size_t i = 0; i < call.arguments.size() && i < call.decl->args.size(); i++) {
                if (call.arguments[i] == nullptr) {
                    continue;
                }

                bind_null_to(call.arguments[i], call.decl->args[i]->type());
            }
        }

        // true when every argument's type is known, so a decision made about them is final rather
        // than premature
        bool arguments_are_determined(const FunctionCallExprNode &call)
        {
            for (const auto *arg : call.arguments) {
                // a hole left by a failed parse cannot be waited on - there is nothing coming that
                // would give it a type, and the diagnostic for it was already reported where it was
                // read
                if (arg == nullptr) {
                    continue;
                }

                if (is_undetermined_type(arg->result_type())) {
                    return false;
                }
            }

            return true;
        }
    }

    std::vector<FunctionDeclNode *> CallResolver::candidates_for(const FunctionCallExprNode &call) const
    {
        // a free call: the namespace it was written in, searched outward by the registry
        if (call.lookup_namespace != nullptr) {
            return _collector.functions.overloads(call.token_function_name.value(), *call.lookup_namespace);
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

        return find_member_functions(receiver_type.get_complex_type(), call.token_function_name.value());
    }

    CallResolver::Result CallResolver::choose_declaration(
        FunctionCallExprNode &call,
        const std::vector<FunctionDeclNode *> &candidates,
        const CodeRef &at,
        bool report)
    {
        const std::string &name = call.token_function_name.value();

        const std::vector<ValueType> argument_types = argument_types_of(call);

        // with a single candidate there is nothing to choose between, so it is taken as written and
        // every judgement about it is left to the passes that specialise in one: the monomorphizer
        // reports an unsatisfied constraint by name, the type checker reports which argument is
        // wrong. pre-filtering here would replace both with "no overload accepts these arguments" -
        // the same reasoning as the arity short-circuit inside match_function
        const bool choosing = candidates.size() > 1;

        // what the call spelled out, if anything: `foo<int32>(...)` names the instance rather than
        // leaving it to inference, so a candidate has to be scored with those in hand - otherwise a
        // template is judged on parameters the call already decided
        const std::vector<ValueType> explicit_type_args = explicit_type_args_of(call);

        std::vector<FunctionCandidate> match_candidates;
        match_candidates.reserve(candidates.size());

        for (auto *candidate : candidates) {
            auto parameter_types = candidate->parameter_types();

            if (choosing && candidate->is_generic()) {
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
                const Instantiation inst = can_instantiate(candidate, argument_types, explicit_type_args);

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
            });
        }

        const auto match = match_function(match_candidates, argument_types, call.arguments);

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
            // result_type() answers as void and is_undetermined_type reads as "no information", so
            // a caller waiting on *this* call is undecidable in turn rather than wrongly decided
            if (!report) {
                return Result::t_pending;
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
            if (call.arguments[i] == nullptr) {
                continue;
            }

            const ValueType expected = call.decl->args[i]->type();
            ExprNode *argument = call.arguments[i];

            // the one fit rule, asked once and handed to both wrappers below rather than re-asked by
            // each - it is the same question about the same pair, and asking it twice let the two
            // answers differ in principle while costing a full member-function walk in practice
            ArgumentFit fit = argument_fit(argument->result_type(), argument, expected);

            // a value whose type declared a conversion to what this parameter wants. before the borrow
            // below rather than after, so a `string::view&` parameter still sees the borrow rule applied
            // to what this produced
            ExprNode *converted = convert_if_wanted(nodes, argument, fit, expected, call.token_function_name);

            // ...which is why the fit is re-asked when, and only when, that wrapping happened: the
            // borrow rule below is about the conversion's *result* now, not about what the caller wrote
            if (converted != argument) {
                fit = argument_fit(converted->result_type(), converted, expected);
            }

            // a place passed to a borrow parameter is coerced to its address here, so codegen sees a
            // uniform AddrOfExprNode instead of sniffing the argument's kind
            call.arguments[i] = borrow_if_wanted(nodes, converted, fit);

            // is_implicitly_convertible rather than ==, so a borrow passed where a nullable pointer
            // is expected does not acquire a cast codegen has no lowering for
            if (!is_implicitly_convertible(call.arguments[i]->result_type(), expected)) {
                call.arguments[i] = &nodes.emplace_back<TypeCastNode>(expected, call.arguments[i], true);
            }
        }

        call.settlement = CallSettlement::t_settled;
    }

    CallResolver::Result CallResolver::settle(
        FunctionCallExprNode &call, NodeCollection &nodes, const CodeRef &at, bool report)
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

        // after the generic gate, so a `T?` parameter is never what a null learns its shape from - the
        // round that rewires `decl` to the instance is the first one with a concrete type to bind. and
        // before the determinedness test below, which is the half of this that un-wedges the fixpoint:
        // bound, the null has a type and the call settles here rather than in the monomorphizer's
        // out-of-rounds sweep
        bind_null_arguments(call);

        // **the fix.** coercing against a type that says nothing cannot tell "no conversion needed"
        // from "no information": the borrow rule declines to wrap, and the cast below it fires for
        // the "remaining mismatch" that was never a mismatch. so wait instead - the round that
        // answers the argument's type asks again
        if (!arguments_are_determined(call)) {
            return Result::t_pending;
        }

        coerce_arguments(call, nodes);
        return Result::t_settled;
    }
};
