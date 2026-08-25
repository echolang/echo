#include <catch2/catch_test_macros.hpp>

#include <AST/ASTNode.h>
#include <AST/ConstDeclNode.h>
#include <AST/ConstRefExprNode.h>
#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/StaticPropertyExprNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/TypeNode.h>
#include <AST/VarDeclNode.h>
#include <AST/VarNode.h>
#include <Compiler/Lsp/LspPositionIndex.h>

#include "helpers.h"

TEST_CASE("a variable use maps to the VarNode, not a miss between tokens", "[lsp]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function main() : void { int32 $n = 1; echo $n; }");

    Compiler::Lsp::PositionIndex index;
    index.build(*bundle);

    AST::File *file = nullptr;
    for (auto &module_ptr : bundle->modules) {
        for (AST::File &candidate : module_ptr->files()) {
            file = &candidate;
        }
    }

    REQUIRE(file != nullptr);

    // `$n` in `echo $n` sits on line 1; the declaration is earlier on the same line
    AST::Node *use = nullptr;
    AST::Node *decl = nullptr;

    for (uint32_t column = 1; column < 60; column++) {
        AST::Node *hit = index.at(file, 1, column);
        const AST::NodeReference ref = AST::make_ref(hit);
        if (ref.has_type<AST::VarNode>() && ref.get_ptr<AST::VarNode>()->decl().name() == "n") {
            use = hit;
        }
        if (ref.has_type<AST::VarDeclNode>() && ref.get_ptr<AST::VarDeclNode>()->name() == "n") {
            decl = hit;
        }
    }

    REQUIRE(decl != nullptr);
    REQUIRE(use != nullptr);
    REQUIRE(use != decl);

    REQUIRE(index.at(file, 1, 1) == nullptr);
}

TEST_CASE("a call maps onto the FunctionCallExprNode whose decl is the callee", "[lsp]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function add(int32 $a) : int32 { return $a; }\n"
        "function main() : void { echo add(1); }");

    Compiler::Lsp::PositionIndex index;
    index.build(*bundle);

    AST::File *file = nullptr;
    for (auto &module_ptr : bundle->modules) {
        for (AST::File &candidate : module_ptr->files()) {
            file = &candidate;
        }
    }

    REQUIRE(file != nullptr);

    AST::FunctionCallExprNode *call = nullptr;
    for (uint32_t column = 1; column < 40; column++) {
        const AST::NodeReference ref = AST::make_ref(index.at(file, 2, column));
        if (ref.has_type<AST::FunctionCallExprNode>()) {
            AST::FunctionCallExprNode *hit = ref.get_ptr<AST::FunctionCallExprNode>();
            if (hit->token_function_name.value() == "add") {
                call = hit;
                break;
            }
        }
    }

    REQUIRE(call != nullptr);
    REQUIRE(call->decl != nullptr);
    REQUIRE(call->decl->func_name() == "add");
}

TEST_CASE("an instantiated generic body is not remapped onto the template's tokens", "[lsp]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function id<T>(T $v) : T { return $v; }\n"
        "function main() : void { echo id<int32>(1); }");

    Compiler::Lsp::PositionIndex index;
    index.build(*bundle);

    AST::File *file = nullptr;
    for (auto &module_ptr : bundle->modules) {
        for (AST::File &candidate : module_ptr->files()) {
            file = &candidate;
        }
    }

    REQUIRE(file != nullptr);

    AST::FunctionDeclNode *template_decl = nullptr;
    for (uint32_t column = 1; column < 30; column++) {
        const AST::NodeReference ref = AST::make_ref(index.at(file, 1, column));
        if (ref.has_type<AST::FunctionDeclNode>()) {
            AST::FunctionDeclNode *hit = ref.get_ptr<AST::FunctionDeclNode>();
            if (hit->func_name() == "id") {
                template_decl = hit;
                break;
            }
        }
    }

    REQUIRE(template_decl != nullptr);
    REQUIRE_FALSE(template_decl->is_instantiated());
}

TEST_CASE("a static property use maps onto the StaticPropertyExprNode", "[lsp]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box { static int32 $count = 0; }\n"
        "function main() : void { echo Box::$count; }");

    Compiler::Lsp::PositionIndex index;
    index.build(*bundle);

    AST::File *file = nullptr;
    for (auto &module_ptr : bundle->modules) {
        for (AST::File &candidate : module_ptr->files()) {
            file = &candidate;
        }
    }

    REQUIRE(file != nullptr);

    AST::StaticPropertyExprNode *access = nullptr;
    for (uint32_t column = 1; column < 50; column++) {
        const AST::NodeReference ref = AST::make_ref(index.at(file, 2, column));
        if (ref.has_type<AST::StaticPropertyExprNode>()) {
            access = ref.get_ptr<AST::StaticPropertyExprNode>();
            break;
        }
    }

    REQUIRE(access != nullptr);
    REQUIRE(access->decl != nullptr);
    REQUIRE(access->decl->name() == "count");
}

TEST_CASE("a type name in a parameter is the TypeNode for that identifier, including after const", "[lsp]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Mat4 { int32 $e; }\n"
        "function matFloats(const Mat4& $m) : void { echo $m->$e; }\n");

    Compiler::Lsp::PositionIndex index;
    index.build(*bundle);

    AST::File *file = nullptr;
    for (auto &module_ptr : bundle->modules) {
        for (AST::File &candidate : module_ptr->files()) {
            file = &candidate;
        }
    }

    REQUIRE(file != nullptr);

    const std::string line = "function matFloats(const Mat4& $m) : void { echo $m->$e; }";
    const size_t col = line.find("Mat4");
    REQUIRE(col != std::string::npos);

    const AST::NodeReference ref = AST::make_ref(index.at(file, 2, static_cast<uint32_t>(col + 1)));
    REQUIRE(ref.has_type<AST::TypeNode>());
    REQUIRE(ref.get_ptr<AST::TypeNode>()->type.get_type_desciption().find("Mat4") != std::string::npos);
}

TEST_CASE("a callable type keeps the start-of-type token", "[lsp]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function take(function<int32()> $cb) : void { }\n");

    AST::TypeNode *written = nullptr;
    for (auto &module_ptr : bundle->modules) {
        for (AST::TypeNode *node : module_ptr->nodes.of_type<AST::TypeNode>()) {
            if (node->type.is_callable()) {
                written = node;
            }
        }
    }

    REQUIRE(written != nullptr);
    REQUIRE(written->type_token.has_value());
    REQUIRE(written->type_token.value().value() == "function");
}

TEST_CASE("a constant use maps to ConstRefExprNode after expansion", "[lsp]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "const uint32 EXTENSIONS = 0x1F03;\n"
        "function main() : void { echo EXTENSIONS; }\n");

    Compiler::Lsp::PositionIndex index;
    index.build(*bundle);

    AST::File *file = nullptr;
    for (auto &module_ptr : bundle->modules) {
        for (AST::File &candidate : module_ptr->files()) {
            file = &candidate;
        }
    }

    REQUIRE(file != nullptr);

    const std::string use = "function main() : void { echo EXTENSIONS; }";
    const size_t col = use.find("EXTENSIONS");
    REQUIRE(col != std::string::npos);

    const AST::NodeReference ref = AST::make_ref(
        index.at(file, 2, static_cast<uint32_t>(col + 1)));
    REQUIRE(ref.has_type<AST::ConstRefExprNode>());
}
