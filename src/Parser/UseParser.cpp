#include "Parser/UseParser.h"

#include "AST/ASTImport.h"
#include "AST/ASTIssue.h"
#include "AST/ASTFile.h"
#include "AST/UseDeclNode.h"

#include "Parser/ParserCursor.h"

#include <fmt/core.h>
#include <optional>

namespace
{

bool read_path(
    Parser::Payload &payload,
    std::vector<std::string> &parts,
    std::optional<TokenReference> &last
)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type(Token::Type::t_identifier)) {
        payload.collect_unexpected_token(Token::Type::t_identifier);
        return false;
    }

    last.emplace(cursor.current());
    parts.push_back(cursor.current().value());
    cursor.skip();

    while (cursor.is_type(Token::Type::t_namespace_sep)) {
        // `use std::math::{sqrt}` - the `::` opens the group, it is not another path segment
        if (cursor.peek_is_type(1, Token::Type::t_open_brace)) {
            break;
        }

        cursor.skip();

        if (cursor.is_type(Token::Type::t_op_mul)) {
            payload.collector.collect_issue<AST::Issue::InvalidUse>(
                payload.context.code_ref(cursor.current()),
                "There is no star import. Name the namespace, or name the items inside '{ }'.");
            cursor.try_skip_to_next_statement();
            return false;
        }

        if (!cursor.is_type(Token::Type::t_identifier)) {
            payload.collect_unexpected_token(Token::Type::t_identifier);
            return false;
        }

        last.emplace(cursor.current());
        parts.push_back(cursor.current().value());
        cursor.skip();
    }

    return true;
}

bool read_optional_alias(
    Parser::Payload &payload,
    std::string &local_name,
    std::optional<TokenReference> &local_token
)
{
    auto &cursor = payload.cursor;
    if (!cursor.is_type(Token::Type::t_as)) {
        return true;
    }

    cursor.skip();

    if (!cursor.is_type(Token::Type::t_identifier)) {
        payload.collect_unexpected_token(Token::Type::t_identifier);
        return false;
    }

    local_token.emplace(cursor.current());
    local_name = cursor.current().value();
    cursor.skip();
    return true;
}

void record_binding(
    Parser::Payload &payload,
    const std::vector<std::string> &path,
    const std::string &local_name,
    const TokenReference &local_token,
    const TokenSlice &span
)
{
    if (payload.pass != Parser::Pass::t_type_names) {
        return;
    }

    const AST::File &file = AST::file_of(payload.context);

    for (const AST::ImportBinding &existing : file.imports) {
        if (existing.local_name != local_name) {
            continue;
        }

        payload.collector.collect_issue<AST::Issue::DuplicateUse>(
            payload.context.code_ref(local_token),
            fmt::format("'{}' is already imported in this file.", local_name));
        return;
    }

    file.imports.push_back(AST::ImportBinding {
        .path = path,
        .local_name = local_name,
        .target_name = path.empty() ? local_name : path.back(),
        .local_token = local_token,
        .span = span,
    });
}

};

AST::UseDeclNode *Parser::parse_usedecl(Parser::Payload &payload, bool at_file_scope)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type(Token::Type::t_use)) {
        payload.collect_unexpected_token(Token::Type::t_use);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    const auto start = cursor.snapshot();
    cursor.skip();

    if (!at_file_scope) {
        payload.collector.collect_issue<AST::Issue::InvalidUse>(
            payload.context.code_ref(cursor.current()),
            "A 'use' cannot appear inside a body - it is a file-local alias, like 'namespace'.");
        cursor.try_skip_to_next_statement({ Token::Type::t_open_brace });
        if (cursor.is_type(Token::Type::t_open_brace)) {
            int depth = 0;
            while (!cursor.is_done()) {
                if (cursor.is_type(Token::Type::t_open_brace)) {
                    depth++;
                }
                else if (cursor.is_type(Token::Type::t_close_brace)) {
                    depth--;
                    cursor.skip();
                    if (depth == 0) {
                        break;
                    }
                    continue;
                }
                cursor.skip();
            }
        }
        if (cursor.is_type(Token::Type::t_semicolon)) {
            cursor.skip();
        }
        return nullptr;
    }

    std::vector<std::string> prefix;
    std::optional<TokenReference> prefix_last;
    if (!read_path(payload, prefix, prefix_last)) {
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    std::string spelling = "use " + AST::join_namespace_path(prefix);

    if (cursor.is_type_sequence(0, { Token::Type::t_namespace_sep, Token::Type::t_open_brace })) {
        cursor.skip();
    }

    if (cursor.is_type(Token::Type::t_open_brace)) {
        cursor.skip();

        bool any = false;
        bool expect_item = true;

        while (!cursor.is_done() && !cursor.is_type(Token::Type::t_close_brace)) {
            if (!expect_item) {
                if (!cursor.is_type(Token::Type::t_comma)) {
                    payload.collect_unexpected_token(Token::Type::t_comma);
                    cursor.try_skip_to_next_statement();
                    return nullptr;
                }
                cursor.skip();
                if (cursor.is_type(Token::Type::t_close_brace)) {
                    break;
                }
            }

            std::vector<std::string> item_path;
            std::optional<TokenReference> item_last;
            if (!read_path(payload, item_path, item_last)) {
                cursor.try_skip_to_next_statement();
                return nullptr;
            }

            std::vector<std::string> full = prefix;
            full.insert(full.end(), item_path.begin(), item_path.end());

            std::string local_name = item_path.back();
            std::optional<TokenReference> local_token = item_last;
            if (!read_optional_alias(payload, local_name, local_token)) {
                cursor.try_skip_to_next_statement();
                return nullptr;
            }

            const auto item_end = cursor.snapshot();
            record_binding(payload, full, local_name, local_token.value(), cursor.slice(start, item_end));
            any = true;
            expect_item = false;
        }

        if (!cursor.is_type(Token::Type::t_close_brace)) {
            payload.collect_unexpected_token(Token::Type::t_close_brace);
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        cursor.skip();

        if (!any) {
            payload.collector.collect_issue<AST::Issue::InvalidUse>(
                payload.context.code_ref(cursor.slice(start, cursor.snapshot())),
                "A grouped 'use' has to name at least one item.");
        }

        spelling += "::{...}";
    }
    else {
        std::string local_name = prefix.back();
        std::optional<TokenReference> local_token = prefix_last;
        if (!read_optional_alias(payload, local_name, local_token)) {
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        if (local_name != prefix.back()) {
            spelling += " as " + local_name;
        }

        const auto end = cursor.snapshot();
        record_binding(payload, prefix, local_name, local_token.value(), cursor.slice(start, end));
    }

    if (!cursor.is_type(Token::Type::t_semicolon)) {
        payload.collect_unexpected_token(Token::Type::t_semicolon);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    cursor.skip();

    auto &node = payload.context.emplace_node<AST::UseDeclNode>(
        cursor.slice(start, cursor.snapshot()), std::move(spelling));

    if (payload.pass == Pass::t_bodies && payload.context.scope_ptr != nullptr) {
        payload.context.scope().children.push_back(AST::make_ref(node));
    }

    return &node;
}
