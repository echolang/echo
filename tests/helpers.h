#include <Parser/ParserCursor.h>
#include <Parser/ModuleParser.h>
#include <AST/ASTBundle.h>
#include <AST/ExprNode.h>

#include <memory>
#include <string>
#include <vector>

#define REQUIRE_NODE_DESC(content, expected_desc) \
    REQUIRE(EchoTests::tests_make_node_description(content) == expected_desc)

#define REQUIRE_NODE_DESC_EXPR(content, expected_desc) \
    REQUIRE(EchoTests::tests_make_node_description_expr(content) == expected_desc)

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
    
    AST::Module tests_make_tokenized_module(std::string content);

    std::unique_ptr<AST::Bundle> tests_make_parsed_bundle(std::string content);

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

    // did any diagnostic mention this? the loose counterpart to assert_code_emits_issue, which
    // compares a whole message
    bool has_issue_containing(const AST::Bundle &bundle, const std::string &needle);
}


