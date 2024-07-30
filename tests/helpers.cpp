#include "helpers.h"

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

AST::Module EchoTests::tests_make_module_with_content(std::string content)
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
