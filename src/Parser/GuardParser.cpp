#include "Parser/GuardParser.h"

#include "Parser/ExprParser.h"
#include "Parser/ScopeParser.h"
#include "Parser/TypeParser.h"

#include "AST/ASTControlFlow.h"
#include "AST/ASTNullability.h"
#include "AST/GuardNode.h"
#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"

#include <fmt/core.h>

AST::GuardNode *Parser::parse_guard(Parser::Payload &payload, AST::ScopeNode *scope)
{
    auto &cursor = payload.cursor;
    auto guard_token = cursor.current();
    cursor.skip(); // `guard`

    // the declared type is optional, exactly as it is in an ordinary declaration - `guard $n = ...` is
    // as legitimate as `guard Node $n = ...`, and infers from the *unwrapped* initializer below
    AST::TypeNode *declared_type = nullptr;
    if (!cursor.is_type(Token::Type::t_varname)) {
        declared_type = Parser::parse_type(payload);

        if (declared_type == nullptr) {
            cursor.try_skip_to_next_statement();
            return nullptr;
        }
    }

    if (!cursor.is_type(Token::Type::t_varname)) {
        payload.collect_unexpected_token(Token::Type::t_varname);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto name_token = cursor.current();
    cursor.skip();

    if (!cursor.is_type(Token::Type::t_assign)) {
        payload.collect_unexpected_token(Token::Type::t_assign);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }
    cursor.skip();

    // the initializer is parsed with **no expected type**, deliberately. the declared type is the
    // *unwrapped* one - `guard Node $n = ...` wants a `Node?` on the right - so handing it down as the
    // expectation would bind a `null` literal to the wrong shape and would tell `&$obj` to produce a
    // borrow where a weak was meant
    auto *init = Parser::parse_expr(payload);
    if (init == nullptr) {
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // a weak is upgraded here, once, through the one function that decides it for all three forms. from
    // this line on the initializer is an ordinary nullable and nothing below knows what a weak is
    init = AST::optional_operand_of(init, payload.context.module, guard_token);

    const AST::ValueType init_type = init->result_type();

    // **a guard that cannot fail is a bug, not a no-op.** it reads as a claim that the value might be
    // absent, and if it never is then either the type is wrong or the guard is - either way the author
    // wants to know. an undetermined type passes through: the monomorphizer reports whatever never
    // resolved, and judging it here would be a round too early
    if (AST::is_certainly_present(init_type)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(guard_token),
            fmt::format(
                "'guard' needs a value that may be absent, and '{}' always is one - write '{}?' if it "
                "may not be, or drop the guard",
                init_type.get_type_desciption(), init_type.get_type_desciption()));
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // the binding holds the **non-null** type, which is the whole point of the form: from here on `$n` is
    // an ordinary local that certainly has a value, and every later read of it is unchecked
    const AST::ValueType payload_type = AST::unwrapped_type_of(init_type);

    if (declared_type == nullptr) {
        declared_type = &payload.context.emplace_node<AST::TypeNode>(payload_type);
    }
    // a *written* type is checked against what is actually inside the nullable, here rather than
    // downstream: AST::TypeChecker skips its usual fit rule for this declaration - the whole point is that
    // the initializer is one level more nullable - so this is the only place the two are both in hand
    //
    // is_implicitly_convertible rather than equality, so `guard int64 $v = lookup($k)` over an `int32?`
    // widens the way an ordinary declaration would
    else if (!AST::is_undetermined_type(payload_type)
        && !AST::is_implicitly_convertible(payload_type, declared_type->type)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(name_token),
            fmt::format(
                "'{}' cannot hold the '{}' inside a '{}'",
                declared_type->type.get_type_desciption(),
                payload_type.get_type_desciption(),
                init_type.get_type_desciption()));
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto &decl = payload.context.emplace_node<AST::VarDeclNode>(name_token, declared_type);
    decl.init_expr = init;

    // see VarDeclNode::binds_unwrapped - the one bit that tells the type checker this declaration's
    // initializer is legitimately one level more nullable than the declaration is
    decl.binds_unwrapped = true;

    // **into the enclosing scope**, not into the else arm and not into a scope of its own. that is what
    // makes the rest of the block able to read it, and it is also what means no new lifetime rule is
    // needed: it is a local like any other, and AST::OwnershipPass drops it at the scope's end
    if (scope != nullptr) {
        scope->declare_variable(decl);
    }

    if (!cursor.is_type(Token::Type::t_else)) {
        payload.collect_unexpected_token(Token::Type::t_else);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }
    cursor.skip();

    if (!cursor.is_type(Token::Type::t_open_brace)) {
        payload.collect_unexpected_token(Token::Type::t_open_brace);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto else_brace = cursor.current();
    cursor.skip();

    AST::ScopeNode &else_scope = Parser::parse_scope(payload, else_brace);

    if (!cursor.is_type(Token::Type::t_close_brace)) {
        payload.collect_unexpected_token(Token::Type::t_close_brace);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }
    cursor.skip();

    // **the else arm must not fall through.** a guard's binding is only meaningful on the path where the
    // value was there, so an arm that ran on and rejoined would leave `$n` bound to nothing at all.
    // refused here rather than left to codegen, so it is a located error about the block the author wrote
    if (!AST::scope_always_exits(else_scope)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(else_brace),
            fmt::format(
                "the 'else' of a guard has to leave - end it with 'return', 'break', 'continue' or "
                "'die'. otherwise '{}' would be read after the value it names turned out not to be there",
                name_token.value()));
    }

    return &payload.context.emplace_node<AST::GuardNode>(&decl, &else_scope, guard_token);
}
