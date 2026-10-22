#include "Parser/CollectionLiteralParser.h"

#include "Parser/ExprParser.h"

#include "AST/ASTIssue.h"
#include "AST/ASTSourceToken.h"
#include "AST/ExprNode.h"

namespace
{
    template<typename ParseItem>
    bool parse_bracket_items(Parser::Payload &payload, ParseItem &&parse_item)
    {
        auto &cursor = payload.cursor;

        while (!cursor.is_type(Token::Type::t_close_bracket)) {
            if (!parse_item()) {
                return false;
            }

            if (cursor.is_type(Token::Type::t_comma)) {
                cursor.skip();
                continue;
            }

            break;
        }

        if (!cursor.is_type(Token::Type::t_close_bracket)) {
            payload.collect_unexpected_token(Token::Type::t_close_bracket);
            return false;
        }

        cursor.skip();
        return true;
    }
}

bool Parser::parse_bracketed_expr_list(Parser::Payload &payload, std::vector<AST::ExprNode *> &elements)
{
    return parse_bracket_items(payload, [&]() {
        auto *element = Parser::parse_expr(payload, nullptr);

        if (element == nullptr) {
            return false;
        }

        elements.push_back(element);
        return true;
    });
}

AST::ExprNode *Parser::parse_collection_literal(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;
    const auto bracket_token = cursor.current();
    cursor.skip();

    bool is_map = false;
    bool decided = false;
    std::vector<AST::ExprNode *> elements;
    std::vector<AST::MapLiteralExprNode::Entry> entries;

    const bool closed = parse_bracket_items(payload, [&]() {
        auto *first = Parser::parse_expr(payload, nullptr);

        if (first == nullptr) {
            return false;
        }

        if (cursor.is_type(Token::Type::t_double_arrow)) {
            const auto arrow = cursor.current();
            cursor.skip();

            auto *value = Parser::parse_expr(payload, nullptr);

            if (value == nullptr) {
                return false;
            }

            if (decided && !is_map) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(arrow),
                    "an array literal cannot mix in map entries - every element is a value, or "
                    "every element is 'key => value'.");
            } else {
                is_map = true;
                decided = true;
                entries.push_back({first, value});
            }
        } else {
            if (decided && is_map) {
                const TokenReference *at = AST::source_token_of(*first);
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    at != nullptr ? payload.context.code_ref(*at) : payload.context.code_ref(cursor.current()),
                    "a map literal cannot mix in array elements - every entry is 'key => value'.");
            } else {
                is_map = false;
                decided = true;
                elements.push_back(first);
            }
        }

        return true;
    });

    if (!closed) {
        return nullptr;
    }

    if (is_map) {
        return &payload.context.emplace_node<AST::MapLiteralExprNode>(
            std::move(entries), bracket_token);
    }

    return &payload.context.emplace_node<AST::ArrayLiteralExprNode>(
        std::move(elements), bracket_token);
}
