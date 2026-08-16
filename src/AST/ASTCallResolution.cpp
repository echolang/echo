#include "AST/ASTCallResolution.h"

#include "AST/ASTVariadic.h"

#include "AST/ASTArgumentFit.h"
#include "AST/ASTArrayLiteral.h"
#include "AST/ASTCFunction.h"
#include "AST/ASTCollector.h"
#include "AST/ASTConstness.h"
#include "AST/ASTFunctionMatcher.h"
#include "AST/ASTInstantiation.h"
#include "AST/ASTLiteralTyping.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTNullability.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeCastNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"

#include <fmt/core.h>

#include <algorithm>
#include <cassert>

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
        // **both borrow ranks produce the same node, and that is the point.** the difference between
        // them - whether the operand already has storage or has to be given some - is a question about
        // the operand's shape, which AST::OwnershipPass asks of the tree.
        //
        // Nothing between here and codegen should be able to tell the two apart, and
        // AST::PointerAdjuster's argument arm already routes any AddrOf through adjust_place without
        // asking
        ExprNode *borrow_if_wanted(NodeCollection &nodes, ExprNode *arg, ArgumentFit fit)
        {
            // all four borrow ranks, asked of AST::fit_is_borrow rather than enumerated here: the const
            // it gains is already on the operand's type, and which ranks are borrows is the fit
            // ordering's own question
            if (!fit_is_borrow(fit)) {
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

            // the conversion's `$this` is a borrow, so the receiver is addressed - **unless the
            // argument already is one**, which is the whole of what makes the conversion reachable
            // through a `const T&` parameter. asked of AST::receiver_for_member_call, which owns that
            // rule for every synthesized member call: addressing a borrow a second time builds a
            // `ptr<ptr<T>>`, which unifies against nothing, and the call is then silently never
            // instantiated rather than refused - a failure mode with no room for a second copy
            auto &conversion_call = nodes.emplace_back<FunctionCallExprNode>(
                at, std::vector<ExprNode *>{ receiver_for_member_call(nodes, arg) });

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
        bool bind_destination_typed_arguments(
            FunctionCallExprNode &call, const CoreTypes &core, NodeCollection &nodes,
            Collector &collector, const CodeRef &at)
        {
            bool waiting_on_a_literal = false;

            for (size_t i = 0; i < call.arguments.size() && i < call.decl->args.size(); i++) {
                if (call.arguments[i] == nullptr) {
                    continue;
                }

                const ValueType expected = call.decl->args[i]->type();

                bind_null_to(call.arguments[i], expected);
                bind_function_ref_to(call.arguments[i], expected, collector.functions);

                // **the one destination a shorthand cannot reach at parse time**, which is why it is
                // here rather than only in the expression parser: a parameter's type sits on a
                // declaration nobody had chosen yet when the argument was read
                //
                // nothing is returned into `waiting_on_a_literal`: an unbound shorthand answers `void`
                // from result_type(), so arguments_are_determined below already holds the call for it
                bind_shorthand_to(call.arguments[i], expected);

                if (bind_array_literal_to(call.arguments[i], expected, core)) {
                    waiting_on_a_literal = true;
                }

                // asked only of a literal nobody has typed, so a round that runs again over a settled
                // argument does nothing - and so an *explicit* cast the author wrote is never undone
                if (is_untyped_literal(call.arguments[i])) {
                    const LiteralTyping typing =
                        type_literal_at(call.arguments[i], value_type_of(expected), nodes);
                    const CodeRef here = code_ref_at_literal(at, call.arguments[i]);

                    report_literal_warning(collector, here, typing);

                    if (typing.result == LiteralTyping::Result::t_refused) {
                        report_literal_refusal(collector, here, typing);
                    }
                    else {
                        call.arguments[i] = typing.node;
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
        bool arguments_are_determined(const FunctionCallExprNode &call)
        {
            for (auto *arg : call.arguments) {
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
                        if (element != nullptr && is_undetermined_type(element->result_type())) {
                            return false;
                        }
                    }

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
        // a **static** call: the type names the overload set. asked ahead of the namespace arm because
        // `Type::f()` carries both a written owner and the namespace the parser was standing in, and
        // the owner is the one that decides - falling through would let the registry's outward walk
        // answer with a free `f` from an enclosing scope
        if (call.static_owner.has_complex_type()) {
            return find_static_functions(call.static_owner.get_complex_type(), call.token_function_name.value());
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
        bool report
    )
    {
        const std::string &name = call.token_function_name.value();

        const std::vector<ValueType> argument_types = argument_types_of(call);

        // which of them have no opinion about the instance's name, read once for every candidate below
        const std::vector<bool> argument_defers = argument_defers_of(call);

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
                //
                // **the owner goes in here too, not only at the monomorphizer's ask.** a static
                // overload set over a generic owner is scored against these substituted parameters,
                // and without the seed every candidate is still holding a bare `T` - so they all
                // rank undetermined and tie, and the call never resolves
                const Instantiation inst = can_instantiate(
                    candidate, argument_types, explicit_type_args, call.static_owner, argument_defers);

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
            if (call.arguments[i] == nullptr) {
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

            // once, for the two questions below: a receiver is an AddrOf over a `->` chain, and
            // MemberAccessNode::result_type walks the whole chain to answer
            const ValueType coerced = call.arguments[i]->result_type();

            // **a receiver refused for its const-ness gets no cast.** `ptr<const Foo>` and `ptr<Foo>`
            // are the same value, so there is nothing here for codegen to lower - the cast's only
            // effect would be visitTypeCast reporting "cannot implicitly convert", drowning the
            // located refusal AST::TypeChecker::check_receiver_const words about the same call.
            //
            // asked of the one owner rather than re-derived from the two types, which cannot tell
            // this apart from any other pointee mismatch - and of its predicate half, so the wording
            // is built by the pass that reports it rather than here, per call, to be thrown away
            if (i == 0 && const_receiver_refused(*call.decl, coerced)) {
                continue;
            }

            // is_implicitly_convertible rather than ==, so a borrow passed where a nullable pointer
            // is expected does not acquire a cast codegen has no lowering for
            //
            // **a wrapped optional is the one place those two questions come apart**, and it needs the
            // cast the first test declines to ask for. `int32` reaches `int32?` perfectly legally, so
            // "implicitly convertible" says yes and nothing wraps it - but a `T?` with no spare null
            // value lowers to `{ i1 __has, T }`, a different machine value, so codegen was handed a bare
            // `i32` for a `{ i1, i32 }` parameter and the IR verifier caught it as an internal error.
            // asked of AST::arrival_wraps_optional, which is also what AST::argument_fit ranked this
            // arrival by and what TypeLowering::coerce_value will emit the wrap from - three readers of
            // one question, and a disagreement between them is exactly the internal error above
            if (!is_implicitly_convertible(coerced, expected)
                || arrival_wraps_optional(coerced, expected)) {
                call.arguments[i] = &nodes.emplace_back<TypeCastNode>(expected, call.arguments[i], true);
            }
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
        //
        // an array literal argument is typed here too, and unlike a null it also makes the call wait:
        // what finally reaches the parameter is the declaration AST::OperatorRewriter hoists, not the
        // literal itself, and that rewrite happens at the top of the next round
        if (!bind_destination_typed_arguments(call, _collector.core_types, nodes, _collector, at)) {
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
