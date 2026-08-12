#include "Parser/TestDeclParser.h"

#include "AST/ASTAttributeReader.h"
#include "AST/ASTContext.h"
#include "AST/ASTIssue.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTTest.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeNode.h"
#include "Parser/AttributeParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/ScopeParser.h"
#include "Parser/SymbolParser.h"

#include <fmt/core.h>

namespace
{
    // the display name of the namespace a test's declaration lives in. never written by anybody, and
    // never read out of a diagnostic either - AST::NamespaceManager::retrieve_lexical keeps the
    // discriminator out of the display name, so a diagnostic inside a test body says `tests::test$adds_up`
    constexpr const char *k_tests_namespace = "tests";

    // **the whole of why a test's declaration is not simply in the file's namespace.**
    //
    // a test's name is unique per *file*, deliberately - two files may each declare a `test adds_up`, and
    // making an author prefix every test with its file would be the cost of a rule nobody asked for. But
    // two declarations of one name in one namespace mangle to one symbol, and TypeLowering throws on that
    // rather than emitting it, so the file has to reach the symbol somehow.
    //
    // a lexical namespace answers it with machinery that already exists and already has this shape: its
    // *mangling* name carries `AST::Context::site_discriminator`'s `<file>L<line>C<column>` while its
    // *display* name does not, which is exactly the split wanted. Keyed on the test's own keyword token,
    // so it is unique without coordination and the body pass - the only pass that mints one - cannot
    // collide with itself
    AST::Namespace &test_namespace(Parser::Payload &payload, const TokenReference &test_token)
    {
        return payload.collector.namespaces.retrieve_lexical(
            *payload.context.current_namespace,
            AST::make_declaration_site(test_token),
            k_tests_namespace,
            payload.context.site_discriminator(test_token));
    }

    // `#[group: "..."]` off the declaration's drained attributes, or empty when none was written.
    //
    // read here rather than by whatever runs the tests, because this is where the attribute is and where a
    // refusal has a token to point at - Parser::read_attribute_value drains its refusals into the collector
    std::string read_group(Parser::Payload &payload, AST::FunctionDeclNode &decl)
    {
        AST::AttributeNode *written = decl.attributes.get_first("group");

        if (written == nullptr) {
            return "";
        }

        std::optional<std::string> group = Parser::read_attribute_value(payload, written, "group",
            [](AST::AttributeReader &reader, const AST::AttributeValue &value) {
                return reader.string(value);
            });

        return group.value_or("");
    }
};

bool Parser::starts_testdecl(Parser::Cursor &cursor)
{
    return cursor.is_type(Token::Type::t_test);
}

void Parser::parse_testdecl(Parser::Payload &payload, bool symbol_only)
{
    auto &cursor = payload.cursor;

    const TokenReference test_token = cursor.current();

    cursor.skip(); // the `test` keyword

    if (!cursor.is_type(Token::Type::t_identifier)) {
        payload.collect_unexpected_token(Token::Type::t_identifier);
        cursor.try_skip_to_next_statement();
        return;
    }

    const TokenReference name_token = cursor.current();
    const std::string name = name_token.value();

    cursor.skip(); // the name

    if (!payload.expect_token(Token::Type::t_open_brace)) {
        cursor.try_skip_to_next_statement();
        return;
    }

    const TokenReference body_brace = cursor.current();

    // **the declaration pass walks the body's declarations and mints no node.**
    //
    // there is no signature to publish - nothing resolves to a test - so the only reason to be here at all
    // is the one every block has: a `struct` or a `function` written inside the body has to join the body's
    // lexical namespace before any call to it is read. That namespace is unreachable from outside, which is
    // the whole of how a test's contents stay its own.
    //
    // the attributes are drained and dropped, and that line is load-bearing: an attribute left on the
    // scope's stack is picked up by the next declaration this walk meets, which is the leak
    // Parser::drain_attributes' own call site in FuncDeclParser records
    if (symbol_only) {
        AST::AttributeList discarded;
        Parser::drain_attributes(payload, discarded);

        Parser::parse_declaration_surface(payload, body_brace);
        return;
    }

    // the body pass. a test is a `FunctionDeclNode` from here on, and every pass after this one finds it
    // by walking the declarations it already walks
    AST::FunctionDeclNode &decl = payload.context.emplace_node<AST::FunctionDeclNode>(
        payload.context.make_virtual_token(
            AST::test_function_name(name), Token::Type::t_identifier, name_token),
        test_token);

    decl.member_kind = AST::MemberKind::t_test;
    decl.ast_namespace = &test_namespace(payload, test_token);

    // where it was written, recorded the way every other declaration parser records it - which is also
    // where the runner reads a test's file from, there being no second copy of that answer
    decl.declared_in = AST::origin_at(payload.context);

    // takes nothing and returns nothing, and there is no syntax for either - so both are stated here
    // rather than read
    decl.return_type = &payload.context.emplace_node<AST::TypeNode>(AST::ValueType::make_void());

    Parser::drain_attributes(payload, decl.attributes);

    const std::string group = read_group(payload, decl);

    // **the declaration goes to the file root**, never into whatever scope it was written in: codegen
    // emits bodies from the file root's children and AST::OwnershipPass resolves drops from the same list,
    // which is the rule a nested `function` already follows
    payload.context.declaration_scope().add_funcdecl(decl);

    auto &test_scope = payload.context.emplace_node<AST::ScopeNode>();

    if (!Parser::parse_function_body(payload, decl, test_scope)) {
        return;
    }

    // **unique per file**, asked of this file's records and of nothing else. The registry would have
    // answered it with DuplicateFunctionSignature had a test been registered there, in words about an
    // overload set nobody wrote
    for (const AST::TestDeclaration &existing : payload.context.module.tests) {
        if (existing.name != name || existing.decl->declared_in.file != decl.declared_in.file) {
            continue;
        }

        payload.collector.collect_issue<AST::Issue::DuplicateTestName>(
            payload.context.code_ref(name_token), name);
        return;
    }

    payload.context.module.tests.push_back(AST::TestDeclaration {
        name,
        group,
        &decl
    });
}
