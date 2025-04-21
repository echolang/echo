#include "Parser/LoopControlParser.h"

#include <fmt/core.h>

AST::LoopControlNode *Parser::parse_loop_control(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    const bool is_break = cursor.is_type(Token::Type::t_break);

    if (!is_break && !cursor.is_type(Token::Type::t_continue)) {
        payload.collect_unexpected_token(Token::Type::t_break);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    const auto keyword = cursor.current();
    cursor.skip();

    // **outside a loop it is reported here, and no node is built.**
    //
    // here rather than in a later pass because the parser is the one that knows lexical nesting, and it
    // knows it *before* Parser::parse_typedecl decides whether a constructor body needs an implicit
    // `return $this` - a decision AST::scope_exit_kind makes from the tree this parse is still building.
    // a stray exit left in that tree would say "this body already left" and suppress the return, for a
    // program that is already broken but by a mechanism nobody could trace back to here
    //
    // the second sentence is carried unconditionally rather than gated on "did we cross a function
    // boundary". the gate would be a second fact about loop_depth that has to be kept in step with it,
    // and this is the only sentence that explains the closure case at all
    if (payload.context.loop_depth == 0) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(keyword),
            fmt::format(
                "'{}' is only meaningful inside a loop - there is no loop here to {}. a closure or a "
                "nested function does not inherit the loop it was written in",
                is_break ? "break" : "continue",
                is_break ? "leave" : "go back to"));

        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    if (!cursor.is_type(Token::Type::t_semicolon)) {
        payload.collect_unexpected_token(Token::Type::t_semicolon);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    cursor.skip();

    return &payload.context.emplace_node<AST::LoopControlNode>(
        is_break ? AST::LoopControlKind::t_break : AST::LoopControlKind::t_continue,
        keyword);
}
