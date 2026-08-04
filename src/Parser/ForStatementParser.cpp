#include "Parser/ForStatementParser.h"

#include "Parser/ExprParser.h"
#include "Parser/ScopeParser.h"
#include "Parser/VarDeclParser.h"

AST::ScopeNode *Parser::parse_forstatement(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;
    auto &context = payload.context;

    if (!payload.expect_token(Token::Type::t_for)) {
        return nullptr;
    }

    const TokenReference for_token = cursor.current();
    cursor.skip();

    // **the paren is consumed here, never handed to parse_expr** - Parser::parse_foreach's reason:
    // is_expr_token admits both parens unconditionally, so the shunting yard would be left holding an
    // unbalanced group when the condition stops at the `;`
    if (!payload.expect_token(Token::Type::t_open_paren)) {
        return nullptr;
    }

    // where the header opens, kept for the recovery below
    const auto header_start = cursor.snapshot();
    cursor.skip();

    // **the wrapper, and it is the init's lifetime.** pushed before the init is parsed so `$i` resolves
    // in the condition, the step and the body; popped on every exit below, which is what the lambda is
    // for. no AST::LexicalScope beside it, deliberately - a header declares no type and no function, and
    // a namespace-less header leaves its calls stamped with the enclosing one, which is what they mean
    auto &wrapper = context.emplace_node<AST::ScopeNode>();
    context.push_scope(wrapper);

    // **the whole statement, header and body, is skipped.** the ordinary recovery stops at the next `;`,
    // and a `for` header holds two of them - so it would hand the rest of the header back to the
    // statement dispatch and report the condition and the step as three more unexpected tokens. one
    // mistake, one diagnostic: the header is stepped over as a balanced group from its `(`, and the body
    // as a balanced group from its `{` when there is one
    auto give_up = [&]() -> AST::ScopeNode * {
        context.pop_scope();

        cursor.restore(header_start);
        cursor.skip_balanced_group(Token::Type::t_open_paren, Token::Type::t_close_paren);

        if (cursor.is_type(Token::Type::t_open_brace)) {
            cursor.skip_balanced_group(Token::Type::t_open_brace, Token::Type::t_close_brace);
        }

        return nullptr;
    };

    // **all three clauses are refused the same way**, so the sentence is written once: the three sites
    // differ only in the clause they name and in whether a `while` would have been the loop the author
    // wanted. one wording rather than three copies of its shared half, which is what keeps the
    // diagnostics corpus from drifting apart a golden at a time
    auto missing_clause = [&](const std::string &clause, const std::string &remedy) -> AST::ScopeNode * {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            context.code_ref(cursor.current()),
            "a 'for' needs all three clauses - this one has no " + clause
                + ". write 'for (int32 $i = 0; $i < 10; $i++)'" + remedy);

        return give_up();
    };

    // **the init.** Parser::parse_varexpr reads both shapes a header can hold - `int32 $i = 0` and
    // `$i = 0` over a variable from an enclosing scope - appends the statement to the scope it is given
    // and consumes the `;`. an empty one is refused rather than tolerated: `for (;;)` reads as an
    // infinite loop in every language that has it, and a `for` that declares nothing is a `while`
    if (cursor.is_type(Token::Type::t_semicolon)) {
        return missing_clause("initializer", ", or use 'while' for a loop that initializes nothing.");
    }

    // not checked for failure, and there is nothing to check: it reports its own refusals and recovers to
    // just past the `;`, which is exactly where the condition begins - so a bad init costs one diagnostic
    // rather than swallowing the rest of the header
    parse_varexpr(payload, &wrapper);

    auto &loop = context.emplace_node<AST::ForStatementNode>(for_token);

    // **the condition.** parse_varexpr above has already consumed the first `;`
    if (cursor.is_type(Token::Type::t_semicolon)) {
        return missing_clause("condition", ".");
    }

    loop.condition = parse_expr(payload);

    if (loop.condition == nullptr) {
        return give_up();
    }

    if (!cursor.is_type(Token::Type::t_semicolon)) {
        payload.collect_unexpected_token(Token::Type::t_semicolon);
        return give_up();
    }
    cursor.skip();

    // **the step, in a scope of its own.** a temporary materialized in `$i = next($i)` then dies where
    // the step finishes, through the frame rule every other block already has
    if (cursor.is_type(Token::Type::t_close_paren)) {
        return missing_clause(
            "step", ", or use 'while' for a loop whose condition is the whole of its step.");
    }

    auto &step_scope = context.emplace_node<AST::ScopeNode>();
    context.push_scope(step_scope);

    // it stops on the `)` and leaves it: is_vardecl_end_token accepts a closing paren and
    // should_skip_vardecl_end_token deliberately does not consume one, because the enclosing parse is
    // what checks it. `$i++` desugars to `$i = $i + 1` in there, which is the one owner of that rule
    parse_varexpr(payload, &step_scope);

    context.pop_scope();
    loop.step = &step_scope;

    if (!cursor.is_type(Token::Type::t_close_paren)) {
        payload.collect_unexpected_token(Token::Type::t_close_paren);
        return give_up();
    }
    cursor.skip();

    if (!cursor.is_type(Token::Type::t_open_brace)) {
        payload.collect_unexpected_token(Token::Type::t_open_brace);
        return give_up();
    }

    const TokenReference loop_brace = cursor.current();
    cursor.skip();

    {
        // **the body only**, exactly as Parser::parse_whilestatement wraps its own: a `break` written in
        // the condition or the step - neither of which is a place one can be written - would belong to an
        // enclosing loop rather than to this one
        AST::LoopScope loop_depth(context, context.loop_depth + 1);

        loop.loop_scope = &parse_scope(payload, loop_brace);
    }

    if (!cursor.is_type(Token::Type::t_close_brace)) {
        payload.collect_unexpected_token(Token::Type::t_close_brace);
        return give_up();
    }
    cursor.skip();

    wrapper.children.push_back(AST::make_ref(loop));
    context.pop_scope();

    return &wrapper;
}
