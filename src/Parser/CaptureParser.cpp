#include "Parser/CaptureParser.h"

#include "AST/ASTValueType.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "Parser/FuncDeclParser.h"

#include <fmt/core.h>

#include <cassert>
#include <optional>
#include <string>
#include <vector>

static const AST::ClosureExprNode::Capture *listed_capture(
    const AST::ClosureExprNode &closure,
    const std::string &name
)
{
    if (!closure.capture_list.has_value()) {
        return nullptr;
    }

    for (const AST::ClosureExprNode::Capture &entry : *closure.capture_list) {
        if (entry.name.value() == name) {
            return &entry;
        }
    }

    return nullptr;
}

static bool refuse_unlisted(
    Parser::Payload &payload,
    const AST::ClosureExprNode &closure,
    const TokenReference &at
)
{
    const bool empty = closure.capture_list->empty();
    const bool here = &closure == payload.context.current_closure();
    const std::string name = at.value();

    std::string message;

    if (empty) {
        message = here
            ? fmt::format(
                "'{}' cannot be captured: this closure was written `function[]()` and captures "
                "nothing.",
                name)
            : fmt::format(
                "'{}' cannot be captured through a closure that captures nothing (`function[]()`). "
                "Capture it in that closure, or pass it as an argument.",
                name);
    }
    else {
        message = here
            ? fmt::format("'{}' is not in this closure's capture list", name)
            : fmt::format("'{}' is not in an enclosing closure's capture list", name);
    }

    payload.collector.collect_issue<AST::Issue::GenericError>(
        payload.context.code_ref(at),
        message);
    return false;
}

static AST::ExprNode *environment_read(
    Parser::Payload &payload,
    AST::ClosureExprNode &closure,
    const TokenReference &at
)
{
    AST::VarDeclNode *env_param = closure.decl->args[0];
    auto &env_var = payload.context.emplace_node<AST::VarNode>(env_param, at);
    auto &env_ref = payload.context.emplace_node<AST::VarRefNode>(&env_var);

    return &payload.context.emplace_node<AST::MemberAccessNode>(AST::make_ref(env_ref), at);
}

// seats the property and the creation-site place on the closure at `nest_index`. does not return
// the body's read - that is the same `$__env->name` for every capture, and minting it here would
// hand a parent the child's expression. walks toward the front of the nest for a through-capture
static bool add_capture(
    Parser::Payload &payload,
    size_t nest_index,
    AST::VarDeclNode *vardecl,
    const TokenReference &at,
    size_t boundaries_crossed
)
{
    const std::vector<AST::ClosureExprNode *> &nest = payload.context.closure_nest;

    assert(nest_index < nest.size() && nest[nest_index] != nullptr && nest[nest_index]->decl != nullptr
        && "add_capture called on a named-function wall");

    AST::ClosureExprNode *closure = nest[nest_index];
    const std::string property_name = vardecl->token_varname.value();
    const AST::ClosureExprNode::Capture *listed = listed_capture(*closure, property_name);

    if (closure->capture_list.has_value() && listed == nullptr) {
        return refuse_unlisted(payload, *closure, at);
    }

    if (boundaries_crossed > 1) {
        if (listed != nullptr && listed->mv.has_value()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(*listed->mv),
                fmt::format(
                    "'{}' is not in the frame this closure is created in. Capture it with `mv` on "
                    "the enclosing closure, or pass it as an argument.",
                    property_name));
            return false;
        }

        if (nest_index == 0 || nest[nest_index - 1] == nullptr) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(at),
                fmt::format(
                    "'{}' is declared outside a nested function that encloses this closure. A "
                    "named function has no environment to hand a capture through - pass it as an "
                    "argument, or write the inner body as a closure too.",
                    at.value()));
            return false;
        }

        if (!add_capture(payload, nest_index - 1, vardecl, at, boundaries_crossed - 1)) {
            return false;
        }
    }

    AST::VarDeclNode *env_param = closure->decl->args[0];

    // whether the captured value owns a resource is *not* asked here: a variable's type is only
    // final once the monomorphizer has settled the call it was inferred from. AST::OwnershipPass
    // walks the captured places at the closure expression and classify_copy decides there
    //
    // a declaration whose inference failed has no type node at all, and `type()` would read through the
    // null. unknown rather than a refusal: the failure has already been reported at the declaration, and
    // "no information" is what every pass below reads an unresolved type as - the same answer
    // VarRefNode::result_type gives for the same declaration
    const AST::ValueType captured_type =
        vardecl->has_type() ? vardecl->type() : AST::ValueType::make_unknown();

    const AST::ValueType env_type = env_param->type();
    AST::ComplexType *environment = env_type.get_complex_type();

    // already captured, so the property is reused rather than added twice. two reads of one variable are
    // one capture - which is also what keeps the property indices in step with `captured_values`
    if (!environment->has_property(property_name)) {
        environment->add_property(property_name, captured_type);

        AST::ExprNode *captured_place = nullptr;

        if (boundaries_crossed > 1) {
            // the enclosing environment's property, read in *this* frame - the inner creation
            // site sits inside the outer body, so `$__env->name` of the outer is an ordinary
            // member access here
            AST::ClosureExprNode *outer = nest[nest_index - 1];
            assert(outer != nullptr && outer->decl != nullptr && "through-capture without an enclosing closure");

            AST::VarDeclNode *outer_env = outer->decl->args[0];
            auto &outer_env_var = payload.context.emplace_node<AST::VarNode>(outer_env, at);
            auto &outer_env_ref = payload.context.emplace_node<AST::VarRefNode>(&outer_env_var);
            captured_place = &payload.context.emplace_node<AST::MemberAccessNode>(
                AST::make_ref(outer_env_ref), at);
        }
        else {
            // the place, read in the *enclosing* frame. it is an ordinary VarRef over the outer
            // declaration, and it is evaluated at the closure expression rather than in the body -
            // which is the whole of what "by value" means here
            auto &outer_var = payload.context.emplace_node<AST::VarNode>(vardecl, at);
            auto &outer_ref = payload.context.emplace_node<AST::VarRefNode>(&outer_var);
            captured_place = &outer_ref;
        }

        if (listed != nullptr && listed->mv.has_value()) {
            captured_place = &payload.context.emplace_node<AST::MoveExprNode>(
                captured_place, *listed->mv);
        }

        closure->captured_values.push_back(captured_place);
    }

    return true;
}

AST::ExprNode *Parser::capture_variable(
    Parser::Payload &payload,
    AST::VarDeclNode *vardecl,
    const TokenReference &at,
    size_t boundaries_crossed
)
{
    AST::ClosureExprNode *closure = payload.context.current_closure();

    assert(closure != nullptr && closure->decl != nullptr && !payload.context.closure_nest.empty()
        && "capture_variable called outside a closure body");
    assert(payload.context.closure_nest.back() == closure && "current_closure is not the nest's innermost slot");

    if (!add_capture(payload, payload.context.closure_nest.size() - 1, vardecl, at, boundaries_crossed)) {
        return nullptr;
    }

    return environment_read(payload, *closure, at);
}

bool Parser::parse_capture_list(
    Parser::Payload &payload,
    std::vector<AST::ClosureExprNode::Capture> &captures
)
{
    auto &cursor = payload.cursor;

    assert(cursor.is_type(Token::Type::t_open_bracket) && "parse_capture_list called off a '['");
    cursor.skip();

    while (!cursor.is_type(Token::Type::t_close_bracket)) {
        if (cursor.is_done()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                payload.context.code_ref(cursor.here()),
                Token::Type::t_close_bracket,
                Token::Type::t_unknown);
            Parser::skip_refused_function(payload);
            return false;
        }

        std::optional<TokenReference> mv_token;

        if (cursor.is_type(Token::Type::t_mv)) {
            mv_token.emplace(cursor.current());
            cursor.skip();
        }
        else if (!cursor.is_type(Token::Type::t_varname)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(cursor.current()),
                "a capture list names `$x` or `mv $x`, or is empty to capture nothing");
            Parser::skip_refused_function(payload);
            return false;
        }

        if (!cursor.is_type(Token::Type::t_varname)) {
            payload.collect_unexpected_token(Token::Type::t_varname);
            Parser::skip_refused_function(payload);
            return false;
        }

        const TokenReference name_token = cursor.current();
        cursor.skip();

        for (const AST::ClosureExprNode::Capture &already : captures) {
            if (already.name.value() == name_token.value()) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(name_token),
                    fmt::format("'{}' is already in this capture list", name_token.value()));
                Parser::skip_refused_function(payload);
                return false;
            }
        }

        const AST::ScopeNode::VariableLookup found =
            payload.context.scope().lookup_variable(name_token.value());

        if (found.decl == nullptr) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(name_token),
                fmt::format(
                    "'{}' is not a variable in this scope, so it cannot be captured",
                    name_token.value()));
            Parser::skip_refused_function(payload);
            return false;
        }

        captures.push_back({ name_token, std::move(mv_token) });

        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
        }
        else if (!cursor.is_type(Token::Type::t_close_bracket)) {
            payload.collect_unexpected_token(Token::Type::t_close_bracket);
            Parser::skip_refused_function(payload);
            return false;
        }
    }

    cursor.skip(); // the `]`
    return true;
}

void Parser::report_unused_captures(
    Parser::Payload &payload,
    const AST::ClosureExprNode &closure,
    const AST::ComplexType &environment
)
{
    if (!closure.capture_list.has_value()) {
        return;
    }

    for (const AST::ClosureExprNode::Capture &named : *closure.capture_list) {
        if (!environment.has_property(named.name.value())) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(named.name),
                fmt::format(
                    "'{}' is named in the capture list but never read in this closure",
                    named.name.value()));
        }
    }
}
