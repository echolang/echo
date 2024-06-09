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

    ParserEnv tests_make_parser_env(std::string content);
    
    AST::Module tests_make_module_with_content(std::string content);
}



