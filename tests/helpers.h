#ifndef TESTS_HELPERS_H
#define TESTS_HELPERS_H

#pragma once

#include <Parser/ParserCursor.h>
#include <Parser/ModuleParser.h>
#include <AST/ASTBundle.h>
#include <AST/ExprNode.h>
#include <Compiler/CompilerOptions.h>

#include <memory>
#include <string>
#include <vector>

#define REQUIRE_NODE_DESC(content, expected_desc) \
    REQUIRE(EchoTests::tests_make_node_description(content) == expected_desc)

#define REQUIRE_NODE_DESC_EXPR(content, expected_desc) \
    REQUIRE(EchoTests::tests_make_node_description_expr(content) == expected_desc)

namespace AST
{
    class TypeDeclNode;
};

namespace EchoTests
{
    struct ParserEnv
    {
        std::unique_ptr<AST::Module> module;
        AST::File &file;
        AST::TokenizedFile &tfile;
        std::unique_ptr<AST::Collector> collector;
        Parser::Payload payload;
    };

    // the options every helper builds a bundle under. **allocation tracking is on**, matching the e2e
    // runner, so a test asking about `mem::live_allocations()` sees the same compiler the corpus does -
    // and the one diagnostic that depends on a flag is not accidentally asserted from the wrong side
    inline Compiler::CompilerOptions tests_compiler_options() {
        Compiler::CompilerOptions options;
        options.track_allocations = true;
        return options;
    }

    ParserEnv tests_make_parser_env(std::string content);

    AST::Module tests_make_tokenized_module(std::string content);

    // **mirrors run_semantic_passes in src/main.cpp**, and is the only place these tests spell the sequence -
    // a pass registered in one and not the other makes every test diverge from the real pipeline, and two
    // copies of the list here made that two chances to forget instead of one
    void run_test_semantic_passes(AST::Bundle &bundle, Compiler::CompilerOptions options);

    std::unique_ptr<AST::Bundle> tests_make_parsed_bundle(std::string content);

    // the same, under options the caller chose. exists for the one diagnostic that depends on a flag:
    // `mem::live_allocations()` is refused without --track-allocations, and the e2e corpus *cannot* test
    // that - it passes the flag to every case, by design, so there is no case shaped like its absence
    std::unique_ptr<AST::Bundle> tests_make_parsed_bundle(
        std::string content, Compiler::CompilerOptions options);

    // parses several files into one module, mirroring a real multi file compile: every file's
    // symbols are collected before any of them is fully parsed
    std::unique_ptr<AST::Bundle> tests_make_parsed_bundle(const std::vector<std::string> &file_contents);

    std::string tests_make_node_description(std::string content);

    std::string tests_make_node_description_expr(std::string content);

    void assert_code_emits_issue(std::string content, std::string expected_issue);

    // shorthand for a primitive type, which every type-level assertion needs
    inline AST::ValueType prim(AST::ValueTypePrimitive p) {
        return AST::ValueType(p);
    }

    // every call site in `m` written with this name, in tree order. the name rather than the decl,
    // because a test usually asks what a *call* resolved to
    std::vector<AST::FunctionCallExprNode *> calls_to(AST::Module &m, const std::string &name);

    // every non-anonymous declaration of this name - a set, not one node, since a name denotes an
    // overload set
    std::vector<AST::FunctionDeclNode *> decls_named(AST::Module &m, const std::string &name);

    // the TypeDeclNode a bare name denotes, or null. scans the arena, so an instantiated clone
    // would show up too
    AST::TypeDeclNode *type_named(AST::Module &m, const std::string &name);

    // every declaration of this type name, in arena order - because one name can denote more than one
    // type: two written namespaces may each hold a `Foo`, and so may two `{ }` blocks
    std::vector<AST::TypeDeclNode *> types_named(AST::Module &m, const std::string &name);

    // how many diagnostics mentioned this? a test asserting that one mistake is reported *once*
    // needs the count, not the yes/no below
    size_t count_issues_containing(const AST::Bundle &bundle, const std::string &needle);

    // did any diagnostic mention this? the loose counterpart to assert_code_emits_issue, which
    // compares a whole message
    bool has_issue_containing(const AST::Bundle &bundle, const std::string &needle);

    // is this declaration a child of the file root's scope? a nested `function` is a scoped
    // declaration and not a closure, so its *name* belongs to its block while the declaration itself
    // is an ordinary top-level one - and it has to be, because codegen emits bodies from this list and
    // AST::OwnershipPass resolves drops from it
    bool is_file_root_child(AST::Module &m, const AST::Node *decl);
};

#endif
