#include <catch2/catch_test_macros.hpp>

#include <AST/ASTConstantExpander.h>
#include <AST/ASTMonomorphizer.h>
#include <AST/ASTAccessPass.h>
#include <AST/ASTPointerAdjuster.h>
#include <AST/ASTTypeChecker.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>

#include "helpers.h"

EchoTests::ParserEnv EchoTests::tests_make_parser_env(std::string content)
{
    auto echomod = std::make_unique<AST::Module>("test", 0);

    // add a pseudo file to the module
    auto &file = echomod->add_file("/tmp/testfile.eco");
    file.set_content(content);

    auto module_parser = Parser::ModuleParser(Compiler::TargetFacts::host());

    // parse the file
    auto tfile = module_parser.make_tokenized_file(*echomod, file);

    auto collector = std::make_unique<AST::Collector>();

    auto payload = module_parser.make_parser_payload(tfile, *echomod, *collector);

    return ParserEnv{
        .module = std::move(echomod),
        .file = file,
        .tfile = tfile,
        .collector = std::move(collector),
        .payload = std::move(payload)
    };
}

AST::Module EchoTests::tests_make_tokenized_module(std::string content)
{
    auto module = AST::Module("test", 0);

    // add a pseudo file to the module
    auto &file = module.add_file("/tmp/testfile.eco");
    file.set_content(content);

    auto module_parser = Parser::ModuleParser(Compiler::TargetFacts::host());

    // parse the file
    auto tfile = module_parser.make_tokenized_file(module, file);

    return module;
}

void EchoTests::run_test_semantic_passes(AST::Bundle &bundle, Compiler::CompilerOptions options)
{
    AST::ConstantExpander(bundle).run();
    AST::Monomorphizer(bundle).run();
    AST::PointerAdjuster(bundle).run();
    AST::AccessPass(bundle).run();
    AST::TypeChecker(bundle, options).run();
}

std::unique_ptr<AST::Bundle> EchoTests::tests_make_parsed_bundle(std::string content)
{
    return tests_make_parsed_bundle(std::move(content), tests_compiler_options());
}

std::unique_ptr<AST::Bundle> EchoTests::tests_make_parsed_bundle(
    std::string content, Compiler::CompilerOptions options)
{
    auto bundle = std::make_unique<AST::Bundle>();
    auto module_handle = bundle->modules.add_module("test");

    // buiild the input payload
    auto input = Parser::ModuleParser::InputPayload {
        .files = {
            Parser::ModuleParser::InputFile("/tmp/testfile.eco", content)
        },
        .module = bundle->modules.get_module(module_handle),
        .collector = bundle->collector
    };

    auto module_parser = Parser::ModuleParser(Compiler::TargetFacts::host());
    module_parser.parse_input(input);

    run_test_semantic_passes(*bundle, options);

    return bundle;
}

std::unique_ptr<AST::Bundle> EchoTests::tests_make_parsed_bundle(const std::vector<std::string> &file_contents)
{
    auto bundle = std::make_unique<AST::Bundle>();
    auto module_handle = bundle->modules.add_module("test");

    std::vector<Parser::ModuleParser::InputFile> files;
    for (size_t i = 0; i < file_contents.size(); i++) {
        files.push_back(Parser::ModuleParser::InputFile("/tmp/testfile" + std::to_string(i) + ".eco", file_contents[i]));
    }

    auto input = Parser::ModuleParser::InputPayload {
        .files = files,
        .module = bundle->modules.get_module(module_handle),
        .collector = bundle->collector
    };

    auto module_parser = Parser::ModuleParser(Compiler::TargetFacts::host());
    module_parser.parse_input(input);

    run_test_semantic_passes(*bundle, EchoTests::tests_compiler_options());

    return bundle;
}

std::string EchoTests::tests_make_node_description(std::string content)
{
    auto bundle = EchoTests::tests_make_parsed_bundle(content);
    auto &module = bundle->modules.find_module("test");

    return module.files().first()->root->node_description_inner();
}

// does the same as "tests_make_node_description" but will autowrap the content in an "echo" call,
// which will again be removed from the final description
std::string EchoTests::tests_make_node_description_expr(std::string content)
{
    auto desc = tests_make_node_description("echo " + content + ";");
    return desc.substr(10, desc.length() - 17); // remove "call echo(" and "): void"
}

void EchoTests::assert_code_emits_issue(std::string content, std::string expected_issue)
{
    auto bundle = EchoTests::tests_make_parsed_bundle(content);

    REQUIRE(bundle->collector.issues.size() > 0); // no issues found
    
    for (const auto &issue : bundle->collector.issues) {
        if (issue->message().find(expected_issue) != std::string::npos) {
            REQUIRE(issue->message() == expected_issue);
            return;
        }
    }

    // nothing found just compare against the first issue
    REQUIRE(bundle->collector.issues[0]->message() == expected_issue);
}

std::vector<AST::FunctionCallExprNode *> EchoTests::calls_to(AST::Module &m, const std::string &name)
{
    std::vector<AST::FunctionCallExprNode *> out;

    for (auto *call : m.nodes.of_type<AST::FunctionCallExprNode>()) {
        if (call->token_function_name.value() == name) {
            out.push_back(call);
        }
    }

    return out;
}

std::vector<AST::FunctionDeclNode *> EchoTests::decls_named(AST::Module &m, const std::string &name)
{
    std::vector<AST::FunctionDeclNode *> out;

    for (auto *decl : m.nodes.of_type<AST::FunctionDeclNode>()) {
        if (!decl->is_anonymous() && decl->func_name() == name) {
            out.push_back(decl);
        }
    }

    return out;
}

AST::TypeDeclNode *EchoTests::type_named(AST::Module &m, const std::string &name)
{
    for (auto *decl : m.nodes.of_type<AST::TypeDeclNode>()) {
        if (decl->type_name() == name) {
            return decl;
        }
    }

    return nullptr;
}

std::vector<AST::TypeDeclNode *> EchoTests::types_named(AST::Module &m, const std::string &name)
{
    std::vector<AST::TypeDeclNode *> types;

    for (auto *decl : m.nodes.of_type<AST::TypeDeclNode>()) {
        if (decl->type_name() == name) {
            types.push_back(decl);
        }
    }

    return types;
}

size_t EchoTests::count_issues_containing(const AST::Bundle &bundle, const std::string &needle)
{
    size_t count = 0;

    for (const auto &issue : bundle.collector.issues) {
        if (issue->message().find(needle) != std::string::npos) {
            count++;
        }
    }

    return count;
}

bool EchoTests::has_issue_containing(const AST::Bundle &bundle, const std::string &needle)
{
    return count_issues_containing(bundle, needle) > 0;
}

bool EchoTests::is_file_root_child(AST::Module &m, const AST::Node *decl)
{
    for (auto &file : m.files()) {
        if (file.root == nullptr) {
            continue;
        }

        for (auto &child : file.root->children) {
            if (child.node() == decl) {
                return true;
            }
        }
    }

    return false;
}
