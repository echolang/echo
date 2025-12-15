#include "Parser/IfStatementParser.h"

#include "Parser/ExprParser.h"
#include "Parser/ScopeParser.h"

bool Parser::starts_const_if(const Parser::Cursor &cursor)
{
    return cursor.is_type_sequence(0, { Token::Type::t_const, Token::Type::t_if });
}

namespace
{
    // the two arms a branch statement is made of - `(<condition>) { ... }` plus an optional
    // `else { ... }` / `else if ...` / `else const if ...` - read once for both spellings.
    //
    // one owner because the *only* difference between `if` and `const if` is the node the arms hang on and
    // the keyword that opened it. the else-continues-with-a-branch rule in particular must not be written
    // twice: such an `else` has no brace of its own and therefore opens no lexical namespace, and two
    // copies of that would be two answers to which braces mint one - which is what AST::ConstFolding's
    // splice relies on when it lifts an arm one level up the tree
    struct BranchArms
    {
        AST::ExprNode *condition = nullptr;
        AST::ScopeNode *if_scope = nullptr;
        AST::ScopeNode *else_scope = nullptr;
    };

    // **the cursor is expected to sit on the `if`.** whichever keyword preceded it is the caller's to
    // consume, which is what keeps `const` out of this function entirely.
    //
    // false when it has reported and recovered, in which case nothing in `out` is usable
    bool parse_branch_arms(Parser::Payload &payload, BranchArms &out)
    {
        if (!payload.cursor.is_type(Token::Type::t_if)) {
            payload.collect_unexpected_token(Token::Type::t_if);
            payload.cursor.try_skip_to_next_statement();
            return false;
        }

        payload.cursor.skip(); // skip the "if" token

        out.condition = Parser::parse_expr(payload);

        if (!out.condition) {
            payload.collect_unexpected_token(Token::Type::t_unknown);
            payload.cursor.try_skip_to_next_statement();
            return false;
        }

        // as long as we do not support one line if statements we need to have a scope
        if (!payload.cursor.is_type(Token::Type::t_open_brace)) {
            payload.collect_unexpected_token(Token::Type::t_open_brace);
            payload.cursor.try_skip_to_next_statement();
            return false;
        }

        auto if_brace = payload.cursor.current();
        payload.cursor.skip(); // skip the opening brace

        out.if_scope = &Parser::parse_scope(payload, if_brace);

        // expect a closing brace
        if (!payload.cursor.is_type(Token::Type::t_close_brace)) {
            payload.collect_unexpected_token(Token::Type::t_close_brace);
            payload.cursor.try_skip_to_next_statement();
            return false;
        }

        payload.cursor.skip(); // skip the closing brace

        if (!payload.cursor.is_type(Token::Type::t_else)) {
            return true;
        }

        payload.cursor.skip(); // skip the "else" token

        // an `else` followed by another branch - `else if` or `else const if` - has no brace of its own:
        // the nested branch is the whole body, so there is no block here and therefore no lexical scope to
        // open. **both spellings**, because a missing second test here would demand a brace the writer had
        // no reason to type
        if (!payload.cursor.is_type(Token::Type::t_if) && !Parser::starts_const_if(payload.cursor)) {
            if (!payload.cursor.is_type(Token::Type::t_open_brace)) {
                payload.collect_unexpected_token(Token::Type::t_open_brace);
                payload.cursor.try_skip_to_next_statement();
                return false;
            }

            const auto else_brace = payload.cursor.current();
            payload.cursor.skip(); // skip the opening brace

            out.else_scope = &Parser::parse_scope(payload, else_brace);

            if (!payload.cursor.is_type(Token::Type::t_close_brace)) {
                payload.collect_unexpected_token(Token::Type::t_close_brace);
                payload.cursor.try_skip_to_next_statement();
                return false;
            }

            payload.cursor.skip(); // skip the closing brace

            return true;
        }

        // **the chained arm holds exactly one statement, and reading it here is what makes that true.**
        // this used to go through parse_scope with no block token, and parse_scope reads statements until
        // it meets a `}` or runs out - so everything written *after* an `else if` chain was swallowed into
        // the else arm. silently: `if (true) { echo 1; } else if ($x) { } else { } echo 5;` printed only 1,
        // because the `echo 5` had become part of the branch that was not taken.
        //
        // the wrapper scope opens no lexical namespace, exactly as before: it has no brace of its own, and
        // a chained `else` declares nothing
        auto &chain = payload.context.emplace_node<AST::ScopeNode>();
        chain.parent_ptr = &payload.context.scope();

        // two arms and not one, because AST::make_ref is typed: it reads the tag off `T::node_type` and
        // asserts it against the node's own, so there is no spelling of it that takes an `AST::Node *`
        if (Parser::starts_const_if(payload.cursor)) {
            auto *nested = Parser::parse_const_ifstatement(payload);

            if (nested == nullptr) {
                return false;
            }

            chain.children.push_back(AST::make_ref(nested));
        }
        else {
            auto *nested = Parser::parse_ifstatement(payload);

            if (nested == nullptr) {
                return false;
            }

            chain.children.push_back(AST::make_ref(nested));
        }

        out.else_scope = &chain;

        return true;
    }
}

AST::IfStatementNode *Parser::parse_ifstatement(Parser::Payload &payload)
{
    auto &ifstatement = payload.context.emplace_node<AST::IfStatementNode>();

    // the `if` itself, read before the arms consume it - so a breakpoint on the line the branch was
    // written on lands on the branch rather than on whatever its condition happens to name
    ifstatement.token_if.emplace(payload.cursor.current());

    BranchArms arms;

    if (!parse_branch_arms(payload, arms)) {
        return nullptr;
    }

    ifstatement.condition = arms.condition;
    ifstatement.if_scope = arms.if_scope;
    ifstatement.else_scope = arms.else_scope;

    return &ifstatement;
}

AST::ConstIfNode *Parser::parse_const_ifstatement(Parser::Payload &payload)
{
    if (!starts_const_if(payload.cursor)) {
        payload.collect_unexpected_token(Token::Type::t_const);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // **the `const` is the node's token, not the `if`**: it is what made the promise every refusal about
    // this statement is about, and the token a reader would delete to make the branch a runtime one
    auto &branch = payload.context.emplace_node<AST::ConstIfNode>(payload.cursor.current());

    payload.cursor.skip(); // the `const`, leaving the cursor on the `if` parse_branch_arms wants

    BranchArms arms;

    if (!parse_branch_arms(payload, arms)) {
        return nullptr;
    }

    branch.condition = arms.condition;
    branch.if_scope = arms.if_scope;
    branch.else_scope = arms.else_scope;

    return &branch;
}
