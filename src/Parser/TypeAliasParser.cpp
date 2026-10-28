#include "Parser/TypeAliasParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/TypeParamParser.h"
#include "Parser/TypeParser.h"

#include "AST/ASTFile.h"
#include "AST/ASTImport.h"
#include "AST/ASTIssue.h"
#include "AST/ASTSymbol.h"
#include "AST/TypeNode.h"

#include <fmt/core.h>

#include <optional>

namespace
{
    AST::TypeAliasDeclNode *alias_declared_at(const AST::File &file, const TokenReference &name_token)
    {
        for (AST::TypeAliasDeclNode *alias : file.type_aliases) {
            if (alias->is_declared_at(name_token)) {
                return alias;
            }
        }

        return nullptr;
    }

    void refuse_collisions(Parser::Payload &payload, AST::TypeAliasDeclNode &alias)
    {
        if (alias.ast_namespace == nullptr) {
            return;
        }

        const AST::Symbol *existing = payload.collector.namespaces.find_symbol(alias.name(), *alias.ast_namespace);

        if (existing != nullptr
            && !(existing->node.has_type<AST::TypeAliasDeclNode>()
                && &existing->node.get<AST::TypeAliasDeclNode>() == &alias)) {
            if (const auto previous = existing->declaration_token()) {
                payload.collector.collect_issue<AST::Issue::TypeRedeclaration>(
                    payload.context.code_ref(alias.token_name), alias.name(), previous.value());
            }

            return;
        }

        if (payload.collector.functions.declares(alias.name(), *alias.ast_namespace)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(alias.token_name),
                fmt::format(
                    "'{}' is a function in this namespace already, so it cannot also be a type alias - "
                    "'{}(...)' would mean two things.",
                    alias.name(), alias.name()));
        }
    }
}

bool Parser::starts_type_alias(const Cursor &cursor)
{
    return cursor.is_type(Token::Type::t_identifier)
        && cursor.current().value() == "type"
        && cursor.peek_is_type(1, Token::Type::t_identifier)
        && (cursor.peek_is_type(2, Token::Type::t_assign) || cursor.peek_is_type(2, Token::Type::t_open_angle));
}

void Parser::declare_type_alias(Payload &payload, std::optional<AST::Visibility> visibility)
{
    auto &cursor = payload.cursor;
    const Cursor::Snapshot at = cursor.snapshot();

    cursor.skip(); // `type`

    const TokenReference name_token = cursor.current();
    cursor.skip();

    const AST::File &file = AST::file_of(payload.context);
    AST::TypeAliasDeclNode *alias = alias_declared_at(file, name_token);

    if (alias == nullptr) {
        alias = &payload.context.emplace_node<AST::TypeAliasDeclNode>(name_token);
        alias->ast_namespace = payload.context.current_namespace;
        alias->declared_in = AST::origin_at(payload.context);
        alias->visibility = AST::declaration_visibility(visibility);
        alias->decl_index = at.index;
        alias->decl_end = at.end;
        file.type_aliases.push_back(alias);

        if (payload.collector.namespaces.find_symbol(name_token.value(), *alias->ast_namespace) == nullptr) {
            alias->ast_namespace->push_symbol(std::make_unique<AST::Symbol>(alias));
        }
    }

    if (cursor.is_type(Token::Type::t_open_angle)) {
        Parser::declare_type_parameters(payload, *alias, parse_type_param_list(payload));
    }

    cursor.try_skip_to_next_statement();
}

void Parser::skip_type_alias(Payload &payload)
{
    Parser::take_pending_doc(payload);
    payload.cursor.try_skip_to_next_statement();
}

bool Parser::resolve_file_type_aliases(Payload &payload)
{
    auto &cursor = payload.cursor;
    const Cursor::Snapshot saved = cursor.snapshot();
    AST::Namespace *saved_namespace = payload.context.current_namespace;

    bool progressed = false;

    for (AST::TypeAliasDeclNode *alias : AST::file_of(payload.context).type_aliases) {
        if (alias->resolution != AST::TypeAliasResolution::t_unresolved) {
            continue;
        }

        cursor.restore({ alias->decl_index, alias->decl_end, saved.shr_split_index });
        cursor.skip(); // `type`
        cursor.skip(); // the name

        payload.context.current_namespace = alias->ast_namespace;

        std::optional<AST::TypeParamScope> params;

        if (cursor.is_type(Token::Type::t_open_angle)) {
            params.emplace(Parser::install_type_parameters(payload, *alias, parse_type_param_list(payload)));
        }

        AST::TypeNode *target = nullptr;
        payload.alias_pending = false;

        if (cursor.is_type(Token::Type::t_assign)) {
            cursor.skip();

            if (Parser::can_parse_type(payload)) {
                target = Parser::parse_type(payload);
            }
        }

        params.reset();
        payload.context.current_namespace = saved_namespace;

        if (payload.alias_pending) {
            continue;
        }

        alias->target = target;
        alias->resolution = AST::TypeAliasResolution::t_resolved;
        progressed = true;
    }

    payload.alias_pending = false;
    cursor.restore(saved);

    return progressed;
}

void Parser::mark_cyclic_type_aliases(const AST::File &file)
{
    for (AST::TypeAliasDeclNode *alias : file.type_aliases) {
        if (alias->resolution == AST::TypeAliasResolution::t_unresolved) {
            alias->resolution = AST::TypeAliasResolution::t_cyclic;
        }
    }
}

void Parser::parse_type_alias(Payload &payload, const VisibilityPrefix &visibility, bool in_block)
{
    auto &cursor = payload.cursor;
    const std::string doc = Parser::take_pending_doc(payload);

    if (in_block) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(cursor.current()),
            "A type alias cannot be declared inside a body - it names a type for a whole namespace, so it "
            "belongs at file or namespace scope.");
        cursor.try_skip_to_next_statement();
        return;
    }

    cursor.skip(); // `type`

    const TokenReference name_token = cursor.current();
    cursor.skip();

    AST::TypeAliasDeclNode *alias = alias_declared_at(AST::file_of(payload.context), name_token);

    if (alias == nullptr) {
        cursor.try_skip_to_next_statement();
        return;
    }

    alias->doc_comment = doc;

    std::optional<AST::TypeParamScope> params;

    if (cursor.is_type(Token::Type::t_open_angle)) {
        params.emplace(Parser::install_type_parameters(payload, *alias, parse_type_param_list(payload)));
    }

    if (!cursor.is_type(Token::Type::t_assign)) {
        payload.collect_unexpected_token(Token::Type::t_assign);
        cursor.try_skip_to_next_statement();
        return;
    }

    cursor.skip(); // `=`

    if (!Parser::can_parse_type(payload)) {
        payload.collect_unexpected_token(Token::Type::t_identifier);
        cursor.try_skip_to_next_statement();
        return;
    }

    AST::TypeNode *target = Parser::parse_type(payload);

    if (!cursor.is_type(Token::Type::t_semicolon)) {
        payload.collect_unexpected_token(Token::Type::t_semicolon);
        cursor.try_skip_to_next_statement();
    }
    else {
        cursor.skip();
    }

    if (target != nullptr) {
        alias->target = target;
    }

    if (alias->resolution == AST::TypeAliasResolution::t_cyclic) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(name_token),
            fmt::format(
                "The type alias '{}' names itself - following its target comes back round to '{}'.",
                alias->name(), alias->name()));
    }
    else if (target != nullptr && target->type_token.has_value()) {
        if (auto refusal = AST::bare_generic_type_refusal(target->type)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(target->type_token.value()),
                std::move(refusal.value()));
        }
    }

    refuse_collisions(payload, *alias);

    if (payload.context.scope_ptr != nullptr) {
        payload.context.scope().children.push_back(AST::make_ref(*alias));
    }
}
