#include "Parser/ExtensionParser.h"

#include "AST/ASTIssue.h"
#include "AST/ASTTypeParam.h"
#include "AST/AttributeNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/TypeNode.h"
#include "Parser/AttributeParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/TypeParser.h"

#include <fmt/core.h>
#include <vector>

namespace
{
bool pending_has_implicit(Parser::Payload &payload)
{
    if (payload.context.scope_ptr == nullptr) {
        return false;
    }

    for (AST::AttributeNode *attribute : payload.context.scope().pending_attributes()) {
        if (attribute != nullptr && attribute->attribute_id.value() == "implicit") {
            return true;
        }
    }

    return false;
}

void skip_extension_body(Parser::Cursor &cursor)
{
    if (cursor.is_type(Token::Type::t_open_brace)) {
        cursor.skip_balanced_group(Token::Type::t_open_brace, Token::Type::t_close_brace);
    }
    else {
        cursor.try_skip_to_next_statement();
    }
}

// a constructor, destructor, operator or property has a body full of `;` and `}`. stopping at
// the first of those resumes *inside* it and leaves its closing brace for the extension
void skip_refused_extension_member(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;
    cursor.skip_until({
        Token::Type::t_open_brace,
        Token::Type::t_semicolon,
        Token::Type::t_close_brace,
    });

    if (cursor.is_type(Token::Type::t_open_brace)
        || cursor.is_type(Token::Type::t_semicolon)) {
        Parser::skip_declaration_body(payload);
    }
}

void refuse_leftover_attributes(Parser::Payload &payload)
{
    if (payload.context.scope_ptr == nullptr) {
        return;
    }

    const std::vector<AST::AttributeNode *> leftover =
        payload.context.scope().pending_attributes();

    for (AST::AttributeNode *attribute : leftover) {
        if (attribute == nullptr) {
            continue;
        }

        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(attribute->attribute_tokens),
            "an attribute belongs to a declaration, and an 'extension' block is not one. "
            "write it on the conversion inside");
    }

    Parser::discard_attributes(payload);
}

void parse_extension_body(Parser::Payload &payload, AST::TypeDeclNode &owner)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type(Token::Type::t_open_brace)) {
        payload.collect_unexpected_token(Token::Type::t_open_brace);
        cursor.try_skip_to_next_statement();
        return;
    }

    cursor.skip();

    const AST::ValueType self_value_type = owner.value_type();
    auto &self_type_node = payload.context.emplace_node<AST::TypeNode>(
        AST::ValueType::make_pointer(self_value_type, false));
    auto &self_const_type_node = payload.context.emplace_node<AST::TypeNode>(
        AST::ValueType::make_pointer(AST::ValueType::make_const(self_value_type), false));

    AST::SelfScope self_scope(payload.context, &owner, &self_type_node, &self_const_type_node);
    AST::ExtensionScope extension_scope(payload.context);

    while (!cursor.is_done() && !cursor.is_type(Token::Type::t_close_brace)) {
        Parser::take_leading_doc(payload);

        const Parser::VisibilityPrefix visibility =
            Parser::parse_visibility_prefix(payload, Parser::VisibilityPosition::t_declaration);

        if (cursor.is_type(Token::Type::t_hash)) {
            // the modifier goes ahead of the attribute, not behind it. the arm consumes the
            // attribute and comes back around, at which point the prefix is gone - so without
            // this a `public` written here is silently dropped
            Parser::refuse_visibility_prefix(
                payload,
                visibility,
                "A visibility modifier belongs to the declaration, so it goes after the attributes and "
                "directly ahead of the member it is about.");

            Parser::parse_attribute(payload);
            continue;
        }

        if (Parser::starts_funcdecl(cursor)) {
            if (!pending_has_implicit(payload)) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(cursor.current()),
                    "an 'extension' block holds conversions, and this is a method. "
                    "declare it in the type itself");
                Parser::discard_attributes(payload);
                Parser::skip_refused_function(payload);
                continue;
            }

            Parser::parse_funcdecl(payload, Parser::FuncDeclKind::t_normal, visibility);
            continue;
        }

        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(cursor.here()),
            "an 'extension' block holds conversions, and this is not one. "
            "declare it in the type itself");
        Parser::discard_attributes(payload);
        skip_refused_extension_member(payload);
    }

    if (!cursor.is_type(Token::Type::t_close_brace)) {
        payload.collect_unexpected_token(Token::Type::t_close_brace);
        return;
    }

    cursor.skip();
}
};

void Parser::parse_file_scope_extension(
    Payload &payload,
    const std::optional<TokenReference> &block_token,
    const VisibilityPrefix &visibility
)
{
    auto &cursor = payload.cursor;
    const TokenReference at = cursor.current();

    Parser::refuse_visibility_prefix(
        payload,
        visibility,
        "An extension's members write their own visibility - there is nobody for a modifier on the block to describe.");

    refuse_leftover_attributes(payload);

    if (block_token.has_value()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(at),
            "An extension belongs at file or namespace scope.");
        cursor.skip();
        skip_type_shape(cursor);
        skip_extension_body(cursor);
        return;
    }

    cursor.skip(); // `extension`

    AST::TypeNode *named = parse_type(payload);
    if (named == nullptr) {
        skip_extension_body(cursor);
        return;
    }

    const AST::ValueType target = named->type;
    AST::TypeDeclNode *owner = nullptr;

    if (target.has_complex_type()) {
        const AST::ComplexType *layout = target.get_complex_type()->template_or_self();
        owner = layout != nullptr ? layout->declaration : nullptr;
    }

    if (auto unknown = AST::unresolved_type_name_refusal(*named)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(unknown->at), unknown->sentence);
        skip_extension_body(cursor);
        return;
    }

    std::string refusal;
    if (owner == nullptr) {
        refusal = fmt::format(
            "an 'extension' names a struct, a class or an enum, and '{}' is not one",
            target.get_type_desciption());
    }
    else if (owner->is_generic() || AST::contains_type_param(target)) {
        refusal = fmt::format(
            "'{}' is generic, and an implicit conversion on a generic type is not supported yet",
            owner->type_name());
    }
    else if (owner->complex_type().is_interface_kind()
        || owner->complex_type().is_extern_interface_kind()) {
        refusal = fmt::format(
            "an 'extension' names a struct, a class or an enum, and '{}' is an interface",
            owner->type_name());
    }
    else if (owner->complex_type().is_opaque_kind()) {
        refusal = fmt::format(
            "an 'extension' names a struct, a class or an enum, and '{}' is incomplete",
            owner->type_name());
    }

    if (!refusal.empty()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(at), refusal);
        skip_extension_body(cursor);
        return;
    }

    parse_extension_body(payload, *owner);
}
