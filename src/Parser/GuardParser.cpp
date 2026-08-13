#include "Parser/GuardParser.h"

#include "Parser/ExprParser.h"
#include "Parser/ScopeParser.h"
#include "Parser/TypeParser.h"

#include "AST/ASTControlFlow.h"
#include "AST/ASTNullability.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTUnwrap.h"
#include "AST/GuardNode.h"
#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"

#include <fmt/core.h>

AST::GuardNode *Parser::parse_guard(
    Parser::Payload &payload,
    AST::VarDeclNode &binding,
    bool is_const
)
{
    auto &cursor = payload.cursor;
    auto guard_token = cursor.current();
    cursor.skip(); // `guard`

    // the initializer is parsed with **no expected type**, deliberately - and the caller preserves that
    // by branching here before it hands its own type node down. the declared type is the *unwrapped*
    // one - `Node $n = guard <Node?>` - so handing it down as the expectation would bind a `null`
    // literal to the wrong shape and would tell `&$obj` to produce a borrow where a weak was meant
    auto *init = Parser::parse_expr(payload);
    if (init == nullptr) {
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // a weak is upgraded here, once, through the one function that decides it for all three forms. from
    // this line on the initializer is an ordinary nullable and nothing below knows what a weak is
    init = AST::optional_operand_of(init, payload.context.module, guard_token);

    const AST::ValueType init_type = init->result_type();

    // **the plain nullable is decided here and never reaches the fixpoint.** a `T?` is what `guard` has
    // always meant: its payload is a property of the type and nothing later can change it, so this arm
    // is byte for byte what parse_guard did before the unwrapping protocol existed - same tree, same
    // diagnostics, same IR for every program written against it.
    //
    // anything else is a question about a *conformance*, which no parse can answer. AST::GuardLowering
    // asks AST::unwrap_plan_for in the monomorphizer's fixpoint, once the subject's type is settled, and
    // `plan_decided` staying false until then is what stops AST::OwnershipPass walking the body early
    const bool decide_now = init_type.is_nullable();

    if (decide_now) {
        // the binding holds the **non-null** type, which is the whole point of the form: from here on
        // `$n` is an ordinary local that certainly has a value, and every later read of it is unchecked
        const AST::ValueType payload_type = AST::unwrapped_type_of(init_type);

        if (!binding.has_type()) {
            binding.set_type_node(&payload.context.emplace_node<AST::TypeNode>(
                AST::infer_declaration_type(payload_type, is_const)));
        }
        // a *written* type is checked against what is actually inside the nullable, here rather than
        // downstream: AST::TypeChecker skips its usual fit rule for this declaration - the whole point
        // is that the initializer is one level more nullable - so this is the only place the two are
        // both in hand. the wording is AST::guard_payload_refusal's, shared with the fixpoint moment
        else {
            const std::string mismatch =
                AST::guard_payload_refusal(binding.type(), payload_type, init_type);

            if (!mismatch.empty()) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(binding.token_varname), mismatch);
                cursor.try_skip_to_next_statement();
                return nullptr;
            }
        }
    }
    // deferred. a written type is left exactly as the author wrote it; an inferred one gets a
    // placeholder so the `const` they wrote survives to the lowering pass, which is the state an
    // array-literal declaration is already in for the same reason
    else if (!binding.has_type()) {
        binding.set_type_node(&payload.context.emplace_node<AST::TypeNode>(
            AST::infer_declaration_type(AST::ValueType::make_unknown(), is_const)));
    }

    binding.init_expr = init;

    // see VarDeclNode::binds_unwrapped - the one bit that tells the type checker this declaration's
    // initializer is legitimately one level more nullable than the declaration is
    binding.binds_unwrapped = true;

    if (!cursor.is_type(Token::Type::t_else)) {
        payload.collect_unexpected_token(Token::Type::t_else);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }
    cursor.skip();

    // **`else ($e)` binds the reason**, when the subject declares `contract::failable<E>`.
    //
    // **no written type**, deliberately: `E` is whatever the conformance says and there is nothing for an
    // author to usefully narrow it to, so a written one would only owe a second payload-versus-written
    // refusal. easier to add later than to take back.
    //
    // the declaration is minted untyped here and **seeded into the else arm's scope**, which is the whole
    // answer to its lifetime: a seed is registered before the arm's first statement, so `$e` resolves
    // while they are parsed, and it lands as `else_scope->children[0]` so the ordinary frame machinery
    // ends it at the arm's exit. no ownership rule, no codegen and no drop rule - the mechanism a
    // `foreach`'s `$k` and `$el` already use. AST::GuardLowering fills the initializer
    AST::VarDeclNode *failure = nullptr;

    if (cursor.is_type(Token::Type::t_open_paren)) {
        cursor.skip();

        if (!cursor.is_type(Token::Type::t_varname)) {
            payload.collect_unexpected_token(Token::Type::t_varname);
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        failure = &payload.context.emplace_node<AST::VarDeclNode>(cursor.current(), nullptr);
        cursor.skip();

        if (!cursor.is_type(Token::Type::t_close_paren)) {
            payload.collect_unexpected_token(Token::Type::t_close_paren);
            cursor.try_skip_to_next_statement();
            return nullptr;
        }
        cursor.skip();
    }

    if (!cursor.is_type(Token::Type::t_open_brace)) {
        payload.collect_unexpected_token(Token::Type::t_open_brace);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto else_brace = cursor.current();
    cursor.skip();

    std::vector<AST::VarDeclNode *> seeds;

    if (failure != nullptr) {
        seeds.push_back(failure);
    }

    AST::ScopeNode &else_scope = Parser::parse_scope(payload, else_brace, seeds);

    if (!cursor.is_type(Token::Type::t_close_brace)) {
        payload.collect_unexpected_token(Token::Type::t_close_brace);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }
    cursor.skip();

    // **the else arm must not fall through.** a guard's binding is only meaningful on the path where
    // the value was there, so an arm that ran on and rejoined would leave `$n` bound to nothing at all.
    // refused here rather than left to codegen, so it is a located error about the block the author
    // wrote - and refused at *parse* time, this being one of the two askers of AST::scope_always_exits
    // that run while the tree is still being built
    if (!AST::scope_always_exits(else_scope)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(else_brace),
            fmt::format(
                "the 'else' of a guard has to leave - end it with 'return', 'break', 'continue' or "
                "'die'. otherwise '{}' would be read after the value it names turned out not to be there",
                binding.token_varname.value()));
    }

    auto &node = payload.context.emplace_node<AST::GuardNode>(&binding, &else_scope, guard_token);

    node.failure = failure;

    // **a guard that writes `else ($e)` never takes the fast path, even over a `T?`.** that puts "a
    // nullable records only that a value is absent, so there is nothing to bind" in exactly one place -
    // AST::GuardLowering, beside the failable-but-not-declared refusal it belongs with - and it costs
    // nothing, no program written before this existing one
    node.plan_decided = decide_now && failure == nullptr;

    return &node;
}
