#include "AST/ASTCallResolution.h"

#include "AST/ASTArgumentFit.h"
#include "AST/ASTCollector.h"
#include "AST/ASTFunctionMatcher.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTTypeUnify.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeCastNode.h"
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
        ExprNode *borrow_if_wanted(NodeCollection &nodes, ExprNode *arg, const ValueType &expected)
        {
            if (argument_fit(arg->result_type(), arg, expected) != ArgumentFit::t_borrow) {
                return arg;
            }

            return &nodes.emplace_back<AddrOfExprNode>(arg);
        }
    }

    bool CallResolver::arguments_are_determined(const FunctionCallExprNode &call)
    {
        for (const auto *arg : call.arguments) {
            // a hole left by a failed parse cannot be waited on - there is nothing coming that would
            // give it a type, and the diagnostic for it was already reported where it was read
            if (arg == nullptr) {
                continue;
            }

            if (is_undetermined_type(arg->result_type())) {
                return false;
            }
        }

        return true;
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

        std::vector<ValueType> argument_types;
        argument_types.reserve(call.arguments.size());
        for (const auto *arg : call.arguments) {
            argument_types.push_back(arg ? arg->result_type() : ValueType::make_unknown());
        }

        // with a single candidate there is nothing to choose between, so it is taken as written and
        // every judgement about it is left to the passes that specialise in one: the monomorphizer
        // reports an unsatisfied constraint by name, the type checker reports which argument is
        // wrong. pre-filtering here would replace both with "no overload accepts these arguments" -
        // the same reasoning as the arity short-circuit inside match_function
        const bool choosing = candidates.size() > 1;

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
                TypeSubstitution inferred;
                const auto fit = can_instantiate(candidate, argument_types, inferred);

                // the template cannot be instantiated for these arguments at all, so it is not a
                // candidate. this is also how a type constraint filters an overload set
                if (fit == InstantiationFit::t_no) {
                    continue;
                }

                // t_maybe leaves the parameters as written, still mentioning `T`, which the matcher
                // reads as undetermined - the honest answer while the call sits in a template body
                // whose own parameters are not bound yet
                if (fit == InstantiationFit::t_yes) {
                    for (auto &parameter_type : parameter_types) {
                        parameter_type = substitute_type(parameter_type, inferred, _collector.type_registry);
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

            // a place passed to a borrow parameter is coerced to its address here, so codegen sees a
            // uniform AddrOfExprNode instead of sniffing the argument's kind
            call.arguments[i] = borrow_if_wanted(nodes, call.arguments[i], expected);

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
        if (call.settlement == CallSettlement::t_settled) {
            return Result::t_settled;
        }

        if (call.decl == nullptr) {
            const auto candidates = candidates_for(call);

            if (candidates.empty()) {
                return Result::t_unknown_name;
            }

            const auto chosen = choose_declaration(call, candidates, at, report);
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
