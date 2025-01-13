#include "Parser/FuncCallParser.h"
#include "Parser/ExprParser.h"
#include "Parser/TypeParser.h"
#include "Parser/NamespaceParser.h"

#include "AST/FunctionDeclNode.h"
#include "AST/VarDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/ASTCallResolution.h"
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

// takes an already-built call node as far as the types known at parse time allow, through
// AST::CallResolver - which is also what the monomorphizer's fixpoint re-enters to finish it
//
// no candidate list is passed in: the resolver derives one from what the *node* carries, the
// namespace for a free call and the receiver's type for a member one, because it has to be able to
// derive it again in a later round. `t_unknown_name` is handed back rather than reported here, so
// each caller keeps its own wording - "no such function" and "no such member" are different errors,
// located at different tokens
//
// every other diagnostic anchors on `call.token_function_name` rather than on a token passed
// alongside, so a message cannot name a token the node it describes disagrees with
static AST::CallResolver::Result resolve_funccall(Parser::Payload &payload, AST::FunctionCallExprNode &funcall);

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
    //
    // recorded on the node rather than only used here, because the lookup has to be repeatable: a
    // call the parser cannot settle is re-resolved from the fixpoint, and by then there is no
    // enclosing namespace to read off the parser context. one lookup against one store, either way -
    // it used to be two, the enclosing scope chain first and the namespace symbol table as a
    // fallback, which meant `a::foo()` preferred a same-named scope entry over the namespace it
    // explicitly asked for
    funcall.lookup_namespace = requested_namespace ? requested_namespace : payload.context.current_namespace;

    switch (resolve_funccall(payload, funcall)) {
    case AST::CallResolver::Result::t_unknown_name:
        // a name with no declarations anywhere is a different error from a name whose declarations
        // do not answer this call, and only the first one is UnknownFunction
        payload.collector.collect_issue<AST::Issue::UnknownFunction>(payload.context.code_ref(funcname_token), funcname_token.value());
        return nullptr;

    case AST::CallResolver::Result::t_failed:
        return nullptr;

    // settled or pending - either way this is the call, and a pending one is finished by the fixpoint
    default:
        return &funcall;
    }
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

    // no lookup_namespace is stamped: a member call's candidates come from its receiver, which is
    // argument 0 of the node itself, so the resolver can re-derive them in a later round from what
    // the tree already holds
    switch (resolve_funccall(payload, funcall)) {
    case AST::CallResolver::Result::t_unknown_name:
        // a type that declares no such member at all is a different error from one whose overloads do
        // not answer this call, exactly as UnknownFunction is for a free call
        payload.collector.collect_issue<AST::Issue::UnknownMember>(
            payload.context.code_ref(member_token),
            member_token.value(), receiver_type.get_type_desciption());
        return nullptr;

    case AST::CallResolver::Result::t_failed:
        return nullptr;

    default:
        return &funcall;
    }
}

static AST::CallResolver::Result resolve_funccall(Parser::Payload &payload, AST::FunctionCallExprNode &funcall)
{
    // resolution is attempted *here* because the call's type is needed here - `$x = f(1);` takes the
    // variable's type from it, so there is no later pass to leave it to. it is only *finished* here
    // when the arguments' types are already known; when they are not, the monomorphizer's fixpoint
    // finishes it, and that is the same fixpoint that answers what they are
    //
    // so a pending call is kept rather than discarded, and the callers return the node. a null `decl`
    // between here and codegen is a legitimate intermediate state: result_type() answers void, which
    // is_undetermined_type reads as "no information", and TypeChecker, PointerAdjuster and
    // OwnershipPass all already guard the pointer. nothing reaches codegen unsettled, because the
    // finalizing sweep at the end of the fixpoint reports whatever never resolved
    AST::CallResolver resolver(payload.collector);

    return resolver.settle(
        funcall,
        payload.context.module.nodes,
        payload.context.code_ref(funcall.token_function_name),
        // the deferrable diagnostic belongs to the fixpoint, which reports it once when it is out of
        // rounds. reporting it here would reject a program that is perfectly well typed
        false);
}
