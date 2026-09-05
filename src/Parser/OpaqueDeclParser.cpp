#include "Parser/OpaqueDeclParser.h"

#include "AST/ASTDeclarationOrigin.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/TypeDeclParser.h"

#include <fmt/core.h>

namespace
{
// attributes staged for a declaration that is being refused. pass 1 has no scope to drain
// from; the two later passes must still consume them so they cannot attach to whatever
// declaration comes next
void drain_refused_attributes(Parser::Payload &payload)
{
    if (payload.pass == Parser::Pass::t_type_names) {
        return;
    }

    AST::AttributeList refused;
    Parser::drain_attributes(payload, refused);
}
}

AST::TypeDeclNode *Parser::parse_opaque_typedecl(Payload &payload, AST::Visibility visibility)
{
    auto &cursor = payload.cursor;

    // standalone `extern struct Name;` skips the prefix; `struct Name;` inside `extern { }` is
    // already on the type keyword. one function either way
    if (cursor.is_type(Token::Type::t_extern)) {
        cursor.skip();
    }

    if (!starts_typedecl(cursor)) {
        payload.collect_unexpected_token(Token::Type::t_struct);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    const TokenReference keyword = cursor.current();
    const AST::ComplexTypeKind written_kind = typedecl_kind(cursor);
    cursor.skip();

    if (written_kind != AST::ComplexTypeKind::t_struct) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(keyword),
            fmt::format(
                "'extern {}' is not a type Echo can name - write 'extern struct' for an incomplete "
                "C type.",
                AST::type_kind_keyword(written_kind)));
        cursor.skip_until({ Token::Type::t_semicolon, Token::Type::t_close_brace });
        if (cursor.is_type(Token::Type::t_semicolon)) {
            cursor.skip();
        }
        return nullptr;
    }

    if (!payload.expect_token(Token::Type::t_identifier)) {
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    auto name_token = cursor.current();
    cursor.skip();

    // a body, a type-parameter list or a conformance is the complete form, which v1 does not
    // take on this prefix. the wording leaves the door open: a later C-layout struct is this
    // arm starting to accept the body, not a new keyword
    if (cursor.is_type(Token::Type::t_open_angle)
        || cursor.is_type(Token::Type::t_colon)
        || cursor.is_type(Token::Type::t_open_brace)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(name_token),
            "An extern struct is incomplete and has no body - if you know the layout, declare a "
            "plain `struct`.");

        drain_refused_attributes(payload);

        if (cursor.is_type(Token::Type::t_open_brace)) {
            cursor.skip();
            cursor.skip_till_end_of_scope();
        } else {
            cursor.skip_until({ Token::Type::t_semicolon, Token::Type::t_close_brace });
            if (cursor.is_type(Token::Type::t_semicolon)) {
                cursor.skip();
            }
        }
        return nullptr;
    }

    if (!payload.expect_token(Token::Type::t_semicolon)) {
        cursor.try_skip_to_next_statement();
        return nullptr;
    }
    cursor.skip();

    AST::TypeDeclNode *owner_node = payload.context.self_struct_ptr;

    // a nested incomplete type would be a member of an Echo layout, which is not a C name.
    // refused rather than published, so a later `Owner::Inner` does not resolve to one
    if (owner_node != nullptr) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(name_token),
            "An incomplete type cannot be declared inside another type - declare it at file or "
            "namespace scope.");

        drain_refused_attributes(payload);
        return nullptr;
    }

    if (Parser::refuse_type_in_generic_body(payload, name_token)) {
        drain_refused_attributes(payload);
        return nullptr;
    }

    AST::TypeDeclNode *struct_node = nullptr;

    if (auto *structsymbol = payload.collector.namespaces.find_symbol(
            name_token.value(), *payload.context.current_namespace)) {
        struct_node = structsymbol->node.get_ptr<AST::TypeDeclNode>();
    }

    if (struct_node != nullptr && !struct_node->is_declared_at(name_token)) {
        payload.collector.collect_issue<AST::Issue::TypeRedeclaration>(
            payload.context.code_ref(name_token),
            struct_node->type_name(),
            struct_node->declaration_site_token());
        return nullptr;
    }

    if (!struct_node) {
        struct_node = &payload.context.emplace_node<AST::TypeDeclNode>(
            name_token, AST::ComplexTypeKind::t_opaque);
        struct_node->complex_type().declared_in = AST::origin_at(payload.context);
        struct_node->complex_type().visibility = visibility;
    }

    struct_node->set_namespace(payload.context.current_namespace);
    Parser::publish_type_symbol(payload, *payload.context.current_namespace, *struct_node);

    // pass 1 has no scope to drain from - parse_typedecl is not called there for the same
    // reason. attributes land in the two later passes, which is also when unique/atomic/core
    // can be refused at the declaration
    if (payload.pass != Pass::t_type_names) {
        Parser::drain_attributes(payload, struct_node->attributes);
        Parser::bind_type_decl_attributes(payload, *struct_node);
    }

    // the body pass only: the declaration pass has no file root, and a name is already
    // resolvable without this because parse_type_names published it
    if (payload.pass == Pass::t_bodies) {
        payload.context.scope().add_typedecl(*struct_node);
    }

    return struct_node;
}
