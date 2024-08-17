#include "helpers.h"
#include <catch2/catch_test_macros.hpp>

#include <AST/ASTMonomorphizer.h>

EchoTests::ParserEnv EchoTests::tests_make_parser_env(std::string content)
{
    auto echomod = std::make_unique<AST::Module>("test", 0);

    // add a pseudo file to the module
    auto &file = echomod->add_file("/tmp/testfile.eco");
    file.set_content(content);

    auto module_parser = Parser::ModuleParser();

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

    auto module_parser = Parser::ModuleParser();

    // parse the file
    auto tfile = module_parser.make_tokenized_file(module, file);

    return module;
}

std::unique_ptr<AST::Bundle> EchoTests::tests_make_parsed_bundle(std::string content)
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

    auto module_parser = Parser::ModuleParser();
    module_parser.parse_input(input);

    // resolve generics into concrete instances, matching the real compile pipeline
    AST::Monomorphizer(*bundle).run();

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
