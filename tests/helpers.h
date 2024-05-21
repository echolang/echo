#include <Parser/ParserCursor.h>
#include <Parser/ModuleParser.h>

#include <memory>

namespace EchoTests
{
    struct ParserEnv {
        std::unique_ptr<AST::Module> module;
        AST::File &file;
        AST::TokenizedFile &tfile;
        std::unique_ptr<AST::Collector> collector;
        Parser::Payload payload;
    };

    ParserEnv tests_make_parser_env(std::string content)
    {
        auto module = new AST::Module("test", 0);

        // add a pseudo file to the module
        auto &file = module->add_file("/tmp/testfile.eco");
        file.set_content(content);

        auto module_parser = Parser::ModuleParser();

        // parse the file
        auto &tfile = module_parser.make_tokenized_file(*module, file);

        auto collector = new AST::Collector();

        return ParserEnv{
            .module = std::unique_ptr<AST::Module>(module),
            .file = file,
            .tfile = tfile,
            .collector = std::unique_ptr<AST::Collector>(collector),
            .payload = module_parser.make_parser_payload(tfile, *module, *collector)
        };
    }

    std::unique_ptr<AST::Module> tests_make_module_with_content(std::string content)
    {
        auto module = new AST::Module("test", 0);

        // add a pseudo file to the module
        auto &file = module->add_file("/tmp/testfile.eco");
        file.set_content(content);

        auto module_parser = Parser::ModuleParser();

        // parse the file
        auto &tfile = module_parser.make_tokenized_file(*module, file);

        return std::unique_ptr<AST::Module>(module);
    }
}



