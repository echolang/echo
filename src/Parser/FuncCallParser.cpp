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
#include "AST/ASTTypeUnify.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/OperatorNode.h"
#include "AST/TypeCastNode.h"
#include <fmt/core.h>

#include <map>
#include <functional>
#include <unordered_map>

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
    if (payload.cursor.is_type(Token::Type::t_open_angle)) {
        payload.cursor.skip(); // skip '<'

        while (!payload.cursor.is_generic_close()) {
            if (payload.cursor.is_done()) {
                payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(funcname_token), Token::Type::t_close_angle, Token::Type::t_unknown);
                payload.cursor.try_skip_to_next_statement();
                return nullptr;
            }

            auto *type_node = parse_type(payload);
            if (type_node) {
                explicit_type_args.push_back(type_node);
            }

            if (payload.cursor.is_type(Token::Type::t_comma)) {
                payload.cursor.skip();
            }
        }

        payload.cursor.consume_generic_close(); // consume '>' (splitting a '>>' if present)
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
    while (!payload.cursor.is_type(Token::Type::t_close_paren)) {
        if (payload.cursor.is_done()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(funcname_token), Token::Type::t_close_paren, Token::Type::t_unknown);
            payload.cursor.try_skip_to_next_statement();
            return nullptr;
        }

        auto arg = parse_expr(payload);
        if (arg == nullptr) {
            // an issue was already collected by the failed sub-parse; abort this call
            // rather than propagating a null argument into the funccall node
            payload.cursor.try_skip_to_next_statement();
            return nullptr;
        }
        args.push_back(arg);

        if (payload.cursor.is_type(Token::Type::t_comma)) {
            payload.cursor.skip();
        }
    }

    // skip the close parenthesis
    payload.cursor.skip();

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
    // wrong. pre-filtering here would replace both with "no overload accepts these arguments".
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
        return nullptr;

    case AST::FunctionMatch::Outcome::t_ambiguous:
        payload.collector.collect_issue<AST::Issue::AmbiguousCall>(
            payload.context.code_ref(funcname_token),
            fmt::format(
                "The call to '{}' is ambiguous. These overloads all match equally well:{}",
                funcname_token.value(), AST::describe_candidates(match.tied)));
        return nullptr;

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
        return nullptr;
    }

    // generic calls keep pointing at the template here; the monomorphizer resolves the
    // concrete instance and rewrites funcall.decl (and inserts casts) after parsing.
    // coerce arguments only for non-generic decls, whose parameter types are concrete.
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

                // Insert an implicit cast node for any remaining mismatches
                auto &cast = payload.context.emplace_node<AST::TypeCastNode>(to, source, true);
                return &cast;
            };

            auto *coerced = coerce_expr(expr, actual, expected);
            args[i] = coerced;
            funcall.arguments[i] = coerced;
        }
    }
    
    return &funcall;
}
