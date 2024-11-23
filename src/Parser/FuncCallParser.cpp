#include "Parser/FuncCallParser.h"
#include "Parser/ExprParser.h"
#include "Parser/TypeParser.h"
#include "Parser/NamespaceParser.h"

#include "AST/FunctionDeclNode.h"
#include "AST/VarDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/ASTArgumentCoercion.h"
#include "AST/ASTFunctionMatcher.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTTypeUnify.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/OperatorNode.h"
#include "AST/TypeCastNode.h"
#include <fmt/core.h>

#include <cassert>
#include <map>
#include <functional>
#include <unordered_map>

// an optional explicit type-argument list, `<int32, float64>`, consumed through its closing `>`
// answers true when there was none to read at all, false when the tokens are not a list after all
//
// `speculative` is the only difference between the two call forms, and it is a reporting policy, not
// a second grammar: a bare identifier is never a comparison operand so `foo <` is unambiguously a
// list, while a *member* is - `$a->count < 3` reaches this with the cursor on a `<` that is not a
// list at all, and only the caller's `(` test settles it. a speculative failure therefore reports
// nothing and leaves the caller to restore its snapshot
static bool parse_explicit_type_args(
    Parser::Payload &payload,
    const TokenReference &at_token,
    std::vector<AST::TypeNode *> &type_args,
    bool speculative)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type(Token::Type::t_open_angle)) {
        return true;
    }

    cursor.skip(); // the '<'

    while (!cursor.is_generic_close()) {
        // can_parse_type is what ends the list: parse_type never answers null for an arbitrary
        // token, it skips the token and answers `unknown`. ungated, the `<` of `$a->count < 3`
        // consumed every remaining token in the file - one arena-allocated TypeNode each, which
        // outlive the rollback, so a file of such comparisons was quadratic in time and memory
        if (cursor.is_done() || !Parser::can_parse_type(payload)) {
            if (!speculative) {
                payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                    payload.context.code_ref(at_token), Token::Type::t_close_angle, Token::Type::t_unknown);
            }

            return false;
        }

        auto *type_node = Parser::parse_type(payload);
        if (type_node == nullptr) {
            // parse_type has already reported whatever it could not read
            return false;
        }

        type_args.push_back(type_node);

        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
        }
    }

    cursor.consume_generic_close(); // the '>', splitting a '>>' if present

    return true;
}

// the argument list of a call, from just past the `(` through the `)`. answers false having reported,
// leaving the recovery to the caller
//
// shared by both call forms so the "unterminated argument list" diagnostic and the optional-comma
// rule exist once - anything the argument grammar grows later (defaults, named arguments) is written
// here rather than in each
static bool parse_call_arguments(
    Parser::Payload &payload,
    const TokenReference &at_token,
    std::vector<AST::ExprNode *> &args)
{
    auto &cursor = payload.cursor;

    while (!cursor.is_type(Token::Type::t_close_paren)) {
        if (cursor.is_done()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                payload.context.code_ref(at_token), Token::Type::t_close_paren, Token::Type::t_unknown);
            return false;
        }

        auto *arg = Parser::parse_expr(payload);
        if (arg == nullptr) {
            // an issue was already collected by the failed sub-parse; abort this call rather than
            // propagating a null argument into the funccall node
            return false;
        }

        args.push_back(arg);

        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
        }
    }

    cursor.skip(); // the ')'

    return true;
}

bool Parser::starts_call_statement(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    // walk any number of `identifier ::` pairs, so `a::b::foo(` is recognised as readily as
    // `foo(`. the namespace prefix is not consumed here - this only looks
    const size_t offset = peek_past_namespace_prefix(payload);

    if (!cursor.peek_is_type(offset, Token::Type::t_identifier)) {
        return false;
    }

    // `(` is a plain call, `<` an explicitly parameterised one. a bare identifier is never a
    // comparison operand - values carry a `$` - so `foo <` is unambiguous, the same reasoning
    // parse_varexpr relies on
    return cursor.peek_is_type(offset + 1, Token::Type::t_open_paren)
        || cursor.peek_is_type(offset + 1, Token::Type::t_open_angle);
}

AST::FunctionCallExprNode *Parser::parse_funccall(Parser::Payload &payload, const AST::Namespace *requested_namespace)
{
    // a call is `name(` or, with explicit type arguments, `name<...>(`
    if (!payload.cursor.is_type_sequence(0, {Token::Type::t_identifier, Token::Type::t_open_paren}) &&
        !payload.cursor.is_type_sequence(0, {Token::Type::t_identifier, Token::Type::t_open_angle})) {
        payload.collect_unexpected_token(Token::Type::t_identifier);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto funcname_token = payload.cursor.current();

    // skip the function name
    payload.cursor.skip();

    // optional explicit type arguments: name<int, float>(...)
    std::vector<AST::TypeNode *> explicit_type_args;
    if (!parse_explicit_type_args(payload, funcname_token, explicit_type_args, false)) {
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // the open parenthesis is required
    if (!payload.cursor.is_type(Token::Type::t_open_paren)) {
        payload.collect_unexpected_token(Token::Type::t_open_paren);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the open parenthesis
    payload.cursor.skip();

    // parse the arguments
    std::vector<AST::ExprNode *> args;
    if (!parse_call_arguments(payload, funcname_token, args)) {
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto &funcall = payload.context.emplace_node<AST::FunctionCallExprNode>(funcname_token, args);
    funcall.explicit_type_args = explicit_type_args;

    // a qualified call names the namespace to look in; an unqualified one starts at the one it is
    // written in and the registry walks outward from there
    if (!requested_namespace) {
        requested_namespace = payload.context.current_namespace;
    }

    // one lookup, against one store. this used to be two - the enclosing scope chain first and
    // the namespace symbol table as a fallback - which meant `a::foo()` preferred a same-named
    // entry from the scope chain over the namespace it explicitly asked for
    const auto candidates = payload.collector.functions.overloads(funcname_token.value(), *requested_namespace);

    // a name with no declarations anywhere is a different error from a name whose declarations
    // do not answer this call, and only the first one is UnknownFunction
    if (candidates.empty()) {
        payload.collector.collect_issue<AST::Issue::UnknownFunction>(payload.context.code_ref(funcname_token), funcname_token.value());
        return nullptr;
    }

    if (!resolve_funccall(payload, funcall, candidates)) {
        return nullptr;
    }

    return &funcall;
}

AST::FunctionCallExprNode *Parser::parse_member_call(
    Parser::Payload &payload,
    AST::ExprNode *receiver,
    const TokenReference &member_token,
    bool &is_call)
{
    auto &cursor = payload.cursor;

    is_call = false;

    // explicit type arguments are speculative here. a member is a legitimate comparison operand, so
    // `$a->count < 3` reaches this with the cursor on a `<` that is not a type argument list at all -
    // the only thing that settles it is whether a `(` follows the close
    std::vector<AST::TypeNode *> explicit_type_args;
    if (cursor.is_type(Token::Type::t_open_angle)) {
        const auto before_type_args = cursor.snapshot();

        // the list has to be followed immediately by the call itself. anything else and this was a
        // comparison, so the tokens go back exactly as they were - including the '>>' split state,
        // which is why this restores a cursor snapshot rather than counting tokens back
        if (!parse_explicit_type_args(payload, member_token, explicit_type_args, true)
            || !cursor.is_type(Token::Type::t_open_paren)) {
            cursor.restore(before_type_args);
            return nullptr;
        }
    }

    // the caller only enters on a `(` or a `<`, and the `<` branch above either returned or left the
    // cursor on the `(` it required
    assert(cursor.is_type(Token::Type::t_open_paren));

    // committed: from here on this is a call, and a failure is reported rather than reinterpreted
    is_call = true;

    cursor.skip(); // the '('

    // the receiver, as an address. taken here rather than left to the borrow coercion in
    // resolve_funccall because that rule reads a *value* against a borrow parameter - a `ptr<Foo>`
    // receiver would rank as no fit at all, and `$p->m()` would fail where `$p->x` works. `->`
    // reaches through every pointer level, so the derefs are spelled out to match - and the type the
    // walk lands on is the receiver's target type, which is what the member lookup below wants
    AST::ExprNode *self_arg = receiver;
    AST::ValueType receiver_type = receiver->result_type();
    while (receiver_type.is_pointer()) {
        self_arg = &payload.context.emplace_node<AST::DerefExprNode>(self_arg);
        receiver_type = receiver_type.pointee();
    }

    if (!AST::is_place_expression(*self_arg)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(member_token),
            fmt::format(
                "'{}' needs a receiver with storage to be called on - a method takes the address of "
                "the value it is called on, and a temporary has none",
                member_token.value()));
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    std::vector<AST::ExprNode *> args;
    args.push_back(&payload.context.emplace_node<AST::AddrOfExprNode>(self_arg));

    if (!parse_call_arguments(payload, member_token, args)) {
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto &funcall = payload.context.emplace_node<AST::FunctionCallExprNode>(member_token, args);
    funcall.explicit_type_args = explicit_type_args;

    // the receiver's type says where to look - the type the deref walk above landed on, which is
    // AST::target_type_of by construction: `->` follows every pointer level, the same rule
    // MemberAccessNode resolves its base with
    if (!receiver_type.has_complex_type()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(member_token),
            fmt::format(
                "'{}' has no members to call - only a struct or a class has member functions",
                receiver_type.get_type_desciption()));
        return nullptr;
    }

    const auto candidates = AST::find_member_functions(receiver_type.get_complex_type(), member_token.value());

    // a type that declares no such member at all is a different error from one whose overloads do
    // not answer this call, exactly as UnknownFunction is for a free call
    if (candidates.empty()) {
        payload.collector.collect_issue<AST::Issue::UnknownMember>(
            payload.context.code_ref(member_token),
            member_token.value(), receiver_type.get_type_desciption());
        return nullptr;
    }

    if (!resolve_funccall(payload, funcall, candidates)) {
        return nullptr;
    }

    return &funcall;
}

bool Parser::resolve_funccall(
    Parser::Payload &payload,
    AST::FunctionCallExprNode &funcall,
    const std::vector<AST::FunctionDeclNode *> &candidates)
{
    auto &args = funcall.arguments;

    // the token the call node was built with, which is the name the user wrote either way: the
    // function's for a free call, the member's for a member one
    const TokenReference &funcname_token = funcall.token_function_name;

    std::vector<AST::FunctionCandidate> match_candidates;
    match_candidates.reserve(candidates.size());

    std::vector<AST::ValueType> argument_types;
    argument_types.reserve(args.size());

    for (auto *arg : args) {
        argument_types.push_back(arg->result_type());
    }

    // with a single candidate there is nothing to choose between, so it is taken as written and
    // every judgement about it is left to the passes that specialise in one: the monomorphizer
    // reports an unsatisfied constraint by name, the type checker reports which argument is
    // wrong. pre-filtering here would replace both with "no overload accepts these arguments"
    // the same reasoning as the arity short-circuit inside match_function
    const bool choosing = candidates.size() > 1;

    for (auto *candidate : candidates) {
        auto parameter_types = candidate->parameter_types();

        if (choosing && candidate->is_generic()) {
            // score a template against the parameters it would actually be instantiated with,
            // not against the bare `T`. an unsubstituted parameter is undetermined, which the
            // matcher treats as neutral - so `pick<T>(T)` would tie with `pick(int32)` for a
            // float64 argument and lose the non-generic tiebreak, calling the concrete overload
            // through a narrowing conversion when the template matched exactly
            AST::TypeSubstitution inferred;
            const auto fit = AST::can_instantiate(candidate, argument_types, inferred);

            // the template cannot be instantiated for these arguments at all, so it is not a
            // candidate. this is also how a type constraint filters an overload set
            if (fit == AST::InstantiationFit::t_no) {
                continue;
            }

            // t_maybe leaves the parameters as written, still mentioning `T`, which the matcher
            // reads as undetermined - the honest answer while the call sits in a template body
            // whose own parameters are not bound yet
            if (fit == AST::InstantiationFit::t_yes) {
                for (auto &parameter_type : parameter_types) {
                    parameter_type = AST::substitute_type(parameter_type, inferred, payload.collector.type_registry);
                }
            }
        }

        match_candidates.push_back(AST::FunctionCandidate {
            .decl = candidate,
            .parameter_types = std::move(parameter_types),
            .is_generic = candidate->is_generic(),
        });
    }

    const auto match = AST::match_function(match_candidates, argument_types, args);

    switch (match.outcome) {
    case AST::FunctionMatch::Outcome::t_resolved:
        funcall.decl = match.decl;
        break;

    case AST::FunctionMatch::Outcome::t_undecidable:
        // several candidates fit and the arguments that would separate them have no type yet - an
        // unbound `null`, a string literal, a variable typed from a generic call. reported rather
        // than left unresolved: a null decl would travel silently to codegen, and no program
        // could reach this before overloads existed, so a clear error costs nobody anything. the
        // real answer is to re-run this match from the monomorphizer once those types exist
        payload.collector.collect_issue<AST::Issue::AmbiguousCall>(
            payload.context.code_ref(funcname_token),
            fmt::format(
                "The call to '{}' cannot be resolved: the types of its arguments are not known "
                "here, and these overloads all remain possible:{}\nAn explicit cast on the "
                "argument picks one.",
                funcname_token.value(), AST::describe_candidates(match.tied)));
        return false;

    case AST::FunctionMatch::Outcome::t_ambiguous:
        payload.collector.collect_issue<AST::Issue::AmbiguousCall>(
            payload.context.code_ref(funcname_token),
            fmt::format(
                "The call to '{}' is ambiguous. These overloads all match equally well:{}",
                funcname_token.value(), AST::describe_candidates(match.tied)));
        return false;

    case AST::FunctionMatch::Outcome::t_no_viable:
    case AST::FunctionMatch::Outcome::t_no_candidates:
        // t_no_candidates here means generic instantiation filtered every candidate out, so nothing
        // reached the matcher for it to have tied - the declarations that were tried are what the
        // user needs to see either way
        payload.collector.collect_issue<AST::Issue::NoMatchingOverload>(
            payload.context.code_ref(funcname_token),
            fmt::format(
                "No overload of '{}' accepts these arguments. Candidates are:{}",
                funcname_token.value(),
                AST::describe_candidates(match.tied.empty() ? candidates : match.tied)));
        return false;
    }

    // generic calls keep pointing at the template here; the monomorphizer resolves the
    // concrete instance and rewrites funcall.decl (and inserts casts) after parsing
    // coerce arguments only for non-generic decls, whose parameter types are concrete
    // an undecidable call has no decl yet and gets both from the monomorphizer instead
    if (!funcall.decl->is_generic()) {
        for (size_t i = 0; i < args.size() && i < funcall.decl->args.size(); ++i) {
            auto expected = funcall.decl->args[i]->type();
            // a variable passed to a pointer parameter is coerced to its address here, so
            // codegen sees a uniform AddrOfExprNode instead of sniffing the argument's kind
            auto *expr = AST::coerce_arg_to_pointer_param(payload.context.module.nodes, args[i], expected);
            auto actual = expr->result_type();

            auto coerce_expr = [&](AST::ExprNode *source, const AST::ValueType &from, const AST::ValueType &to) -> AST::ExprNode * {
                // is_implicitly_convertible rather than ==, so a borrow passed where a nullable
                // pointer is expected does not acquire a cast codegen has no lowering for
                if (AST::is_implicitly_convertible(from, to)) {
                    return source;
                }

                // insert an implicit cast node for any remaining mismatches
                auto &cast = payload.context.emplace_node<AST::TypeCastNode>(to, source, true);
                return &cast;
            };

            args[i] = coerce_expr(expr, actual, expected);
        }
    }

    return true;
}
