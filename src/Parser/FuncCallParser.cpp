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
#include "AST/ASTImport.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTPlaceExpr.h"
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
// `speculative` is the only difference between the callers, and it is a reporting policy, not a second
// grammar: what settles whether a `<` opened a list is the caller's `(` test, and only a caller that can
// *reinterpret* the tokens needs the failure kept quiet. Two can - `$a->count < 3` and, since compile-time
// constants made a bare identifier a value operand, `LIMIT < $n`. A speculative failure therefore reports
// nothing and leaves the caller to restore its snapshot; a committed one is an error, which is right at a
// statement head where nothing else could have been meant
static bool parse_explicit_type_args(
    Parser::Payload &payload,
    const TokenReference &at_token,
    std::vector<AST::TypeNode *> &type_args,
    bool speculative
)
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
// shared by all three call forms so the "unterminated argument list" diagnostic and the optional-comma
// rule exist once - anything the argument grammar grows later (defaults, named arguments) is written
// here rather than in each. a direct call has its expected types on its declaration and does not know
// them until the call resolves, so it passes none; an indirect call reads them off the callee's
// signature and passes them, which is what types a literal argument by its destination
//
// past the end of `expected_types` an argument types itself - the arity mismatch is the caller's to
// report, where the whole list is in view rather than one argument
static bool parse_call_arguments(
    Parser::Payload &payload,
    const TokenReference &at_token,
    std::vector<AST::ExprNode *> &args,
    const std::vector<AST::ValueType> *expected_types = nullptr
)
{
    auto &cursor = payload.cursor;

    while (!cursor.is_type(Token::Type::t_close_paren)) {
        if (cursor.is_done()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                payload.context.code_ref(at_token), Token::Type::t_close_paren, Token::Type::t_unknown);
            return false;
        }

        AST::TypeNode *expected = nullptr;
        if (expected_types != nullptr && args.size() < expected_types->size()) {
            expected = &payload.context.emplace_node<AST::TypeNode>((*expected_types)[args.size()]);
        }

        auto *arg = Parser::parse_expr(payload, expected);
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

bool Parser::starts_indirect_call_statement(Parser::Cursor &cursor)
{
    return cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_open_paren });
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

    // `(` is a plain call, `<` an explicitly parameterised one. Unambiguous **here** because this is a
    // statement head: a bare identifier can be a comparison operand now that a compile-time constant is
    // one, but `LIMIT < $n;` as a whole statement computes a value nobody reads. In an operand position the
    // `<` is speculative instead - see parse_funccall's out_is_call
    return cursor.peek_is_type(offset + 1, Token::Type::t_open_paren)
        || cursor.peek_is_type(offset + 1, Token::Type::t_open_angle);
}

bool Parser::starts_static_property_statement(Parser::Payload &payload)
{
    // **the one shape a statement can start with that reaches no other arm.** the vardecl branch is
    // anchored on a `$name` at the head, the call branch on an identifier followed by a `(`, and the
    // constant-chain branch on an identifier followed by a `->` - so `Session::$count = 1;` matched
    // none of them and fell to the catch-all, which reported an unexpected identifier
    //
    // the shape itself is Parser::starts_static_property's, which is also what the operand parser
    // measures - so the branch this dispatch takes and the production it hands off to cannot disagree
    return Parser::starts_static_property(payload.cursor);
}

bool Parser::starts_constant_chain_statement(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    // the same namespace walk starts_call_statement does, so `std::io::stdout->` is recognised as
    // readily as a bare `stdout->`
    const size_t offset = peek_past_namespace_prefix(payload);

    if (!cursor.peek_is_type(offset, Token::Type::t_identifier)) {
        return false;
    }

    return cursor.peek_is_type(offset + 1, Token::Type::t_accessorlr);
}

AST::IndirectCallExprNode *Parser::parse_indirect_call(
    Parser::Payload &payload,
    AST::ExprNode *callee,
    const TokenReference &at
)
{
    auto &cursor = payload.cursor;

    assert(cursor.is_type(Token::Type::t_open_paren) && "parse_indirect_call called off an argument list");

    const AST::ValueType callee_type = AST::value_type_of(callee->result_type());

    // reported here rather than in the type checker because the *shape* is what is wrong: `$x(1)` on a
    // non-callable is not a call with bad arguments, it is not a call at all
    if (!callee_type.has_signature()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(at),
            fmt::format(
                "'{}' is a '{}', which cannot be called.",
                at.value(), callee_type.get_type_desciption()));
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    cursor.skip(); // the open paren

    const auto &signature = callee_type.signature();

    std::vector<AST::ExprNode *> arguments;

    // through the one argument-list walk, handing it the signature's parameter types as the
    // destinations its literals are typed against
    if (!parse_call_arguments(payload, at, arguments, &signature.parameter_types)) {
        return nullptr;
    }

    if (arguments.size() != signature.parameter_types.size()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(at),
            fmt::format(
                "'{}' takes {} argument(s), but {} were given.",
                callee_type.get_type_desciption(), signature.parameter_types.size(), arguments.size()));
        return nullptr;
    }

    return &payload.context.emplace_node<AST::IndirectCallExprNode>(callee, std::move(arguments), at);
}

AST::FunctionCallExprNode *Parser::parse_funccall(
    Parser::Payload &payload,
    const AST::Namespace *requested_namespace,
    bool *out_is_call,
    const Parser::CallLookup &lookup
)
{
    // the whole call, so declining can put the name back too - the caller has to be able to read it as
    // something else entirely
    const auto before_call = payload.cursor.snapshot();

    // asking for this is what makes the `<` speculative, exactly as it is in parse_member_call
    const bool speculative = out_is_call != nullptr;
    if (speculative) {
        *out_is_call = false;
    }

    // a call is `name(` or, with explicit type arguments, `name<...>(`
    if (!payload.cursor.is_type_sequence(0, {Token::Type::t_identifier, Token::Type::t_open_paren}) &&
        !payload.cursor.is_type_sequence(0, {Token::Type::t_identifier, Token::Type::t_open_angle})) {
        payload.collect_unexpected_token(Token::Type::t_identifier);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto funcname_token = payload.cursor.current();

    const AST::Namespace *imported_ns = nullptr;
    std::string imported_name;
    const bool may_import = requested_namespace == nullptr
        && !lookup.shorthand_dot.has_value()
        && lookup.static_owner.is_unknown()
        && lookup.constructed_type.is_unknown();
    const bool imported = may_import && AST::apply_item_import(
        AST::file_of(payload.context),
        payload.collector,
        funcname_token.value(),
        imported_ns,
        imported_name);

    // skip the function name
    payload.cursor.skip();

    // optional explicit type arguments: name<int, float>(...)
    //
    // **speculative when the caller can reinterpret**, and that is not a nicety: a bare identifier
    // can be a comparison operand - a compile-time constant is one - so `LIMIT < $n` arrives
    // here with the cursor on a `<` that opens no type argument list. The only thing
    // that settles it is whether a `(` follows the close, which is the rule parse_member_call already
    // lives by for the same reason: `$a->count < 3`
    std::vector<AST::TypeNode *> explicit_type_args;
    const bool type_args_read = parse_explicit_type_args(payload, funcname_token, explicit_type_args, speculative);

    if (!type_args_read || !payload.cursor.is_type(Token::Type::t_open_paren)) {
        // the tokens go back exactly as they were, including the '>>' split state - which is why this
        // restores a snapshot rather than counting tokens back
        if (speculative) {
            payload.cursor.restore(before_call);
            return nullptr;
        }

        // a list that failed to read has already reported itself; a missing `(` after a good one has not
        if (type_args_read) {
            payload.collect_unexpected_token(Token::Type::t_open_paren);
        }

        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // committed: from here on this is a call, and a failure is reported rather than reinterpreted
    if (speculative) {
        *out_is_call = true;
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
    // two lookups - the enclosing scope chain first and the namespace symbol table as a
    // fallback - would make `a::foo()` prefer a same-named scope entry over the namespace it
    // explicitly asked for
    //
    // a **static** call names neither: its candidates come from the type, so the namespace is left
    // null and nothing walks outward from it. that omission is the whole of what stops `Foo::f()`
    // from quietly resolving to an enclosing free `f`. a **shorthand** names neither yet, and leaving
    // both unset is the honest state - not a lookup that found nothing
    if (lookup.shorthand_dot.has_value()) {
        funcall.token_shorthand_dot.emplace(lookup.shorthand_dot.value());
    }
    else if (!lookup.constructed_type.is_unknown()) {
        funcall.constructed_type = lookup.constructed_type;
    }
    else if (!lookup.static_owner.is_unknown()) {
        funcall.static_owner = lookup.static_owner;
    }
    else if (imported) {
        funcall.lookup_namespace = imported_ns;
        funcall.imported_name = std::move(imported_name);
    }
    else {
        funcall.lookup_namespace = requested_namespace ? requested_namespace : payload.context.current_namespace;
    }

    switch (resolve_funccall(payload, funcall)) {
    case AST::CallResolver::Result::t_unknown_name:
        // a shorthand has no owner yet, so there was nothing to search and nothing to be unknown.
        // the call is kept, pending, for the destination to name an owner for - and if none ever
        // does, the monomorphizer's finalizing sweep is what says so, having run out of rounds
        if (lookup.shorthand_dot.has_value()) {
            return &funcall;
        }

        // a type-parameter owner is a not-yet: `T::from(...)` and `T(...)` sit in a template body
        // whose clones carry the concrete type. reporting here is one round too early, the same
        // standing an undetermined receiver already has. `is_unknown()` is the "this is not that
        // kind of call" test - unknown is undetermined, and would swallow every ordinary miss
        if ((!lookup.static_owner.is_unknown() && AST::is_undetermined_type(lookup.static_owner))
            || (!lookup.constructed_type.is_unknown()
                && AST::is_undetermined_type(lookup.constructed_type))) {
            return &funcall;
        }

        // the type is named and its static overload set has nothing by that name. a different
        // sentence from UnknownFunction's, because the search was not a search of any namespace -
        // and reported here rather than left to the fixpoint, the owner already being concrete
        if (lookup.static_owner.has_complex_type()) {
            payload.collector.collect_issue<AST::Issue::UnknownStaticFunction>(
                payload.context.code_ref(funcname_token),
                funcname_token.value(),
                lookup.static_owner.get_type_desciption()
            );
            return nullptr;
        }

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

AST::FunctionCallExprNode *Parser::build_operator_call(
    Parser::Payload &payload,
    const AST::Operator &op,
    AST::OpFixity fixity,
    const TokenReference &at,
    std::vector<AST::ExprNode *> operands
)
{
    for (const auto *operand : operands) {
        if (operand == nullptr) {
            return nullptr;
        }
    }

    // the node itself is AST::build_operator_call_node's, shared with AST::OperatorRewriter - the
    // decorated name, the virtual token and the root namespace are one operator use site's shape,
    // whichever moment builds it
    auto &call = AST::build_operator_call_node(
        payload.context.module, payload.collector, op.spelling, fixity, at, std::move(operands));

    // driven, but not judged. an unresolved operator call is a legitimate intermediate state for the
    // reason the header gives, and the fixpoint's finalizing sweep reports whatever is still
    // unresolved when it runs out of rounds
    resolve_funccall(payload, call);

    return &call;
}

AST::FunctionCallExprNode *Parser::parse_member_call(
    Parser::Payload &payload,
    AST::ExprNode *receiver,
    const TokenReference &member_token,
    bool &is_call
)
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

    // a receiver needs an address, so it needs storage - but a value that has none can be *given* some.
    // AST::OwnershipPass binds a temporary for the address below and destroys it once the call has
    // returned, so a call result is a legal receiver and `$o->get()->size()` reads the way
    // it looks. the predicate also admits a literal and an arithmetic result, which is
    // right in principle - a receiver *is* a borrow argument in position 0, so it should take a value
    // wherever an ordinary borrow parameter would - but unreachable in practice from here: a `->` base is
    // read by parse_postfix_chain, which does not accept a parenthesised group, and a primitive declares
    // no methods to call. so what actually reaches this refusal is unchanged
    //
    // spelled as the one taxonomy rather than as two of its three predicates negated: place,
    // materializable and addressless are exhaustive, so "neither of the first two" *is* the third,
    // and saying so keeps this readable against AST::storage_of when a class is added
    //
    // **a constant reference is the one thing this may not ask about**, and that is a statement about
    // *when* rather than about what: AST::ConstantExpander replaces it with a clone of the constant's
    // expression before anything else in the compiler runs, and whatever that expression is answers
    // for it. this check is the only storage question asked while one is still in the tree, so it is
    // the only place that has to say so - `std::io::stdout->write($t)` is `stream(1)->write($t)` by
    // the time storage means anything, which is an ordinary temporary receiver
    if (self_arg->get_node_type() != AST::NodeType::n_expr_const_ref
        && AST::storage_of(*self_arg) == AST::StorageClass::t_addressless) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(member_token),
            fmt::format(
                "'{}' needs a receiver with storage to be called on - a method takes the address of "
                "the value it is called on, and this expression has none to take",
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

    // **an undetermined receiver is "ask again later", not an error.** the type may be a parameter
    // nothing has substituted, an unsettled call's void, or - the case that made this matter - an
    // element access whose contract AST::OperatorRewriter attaches inside the fixpoint, so
    // `$views[0]->count()` has no receiver type at all while the parser is looking at it
    //
    // the same standing a pending free call already has, and the same reason: `resolve_funccall`'s
    // header says a null `decl` between here and codegen is legitimate, and the fixpoint's
    // finalizing sweep reports whatever never resolved. reporting at parse time is one round too
    // early for every receiver whose type a later round answers
    const bool receiver_is_undetermined = AST::is_undetermined_type(receiver_type);

    // the receiver's type says where to look - the type the deref walk above landed on, which is
    // AST::target_type_of by construction: `->` follows every pointer level, the same rule
    // MemberAccessNode resolves its base with
    if (!receiver_type.has_complex_type() && !receiver_is_undetermined) {
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
        // retryable when the receiver is not typed yet - CallResolver::settle says so itself, and
        // keeping the node is what lets the fixpoint try again
        if (receiver_is_undetermined) {
            return &funcall;
        }

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
