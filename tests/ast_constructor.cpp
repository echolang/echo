#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTConstructor.h>
#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ReturnNode.h>
#include <AST/ScopeNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/TypeNode.h>
#include <AST/VarDeclNode.h>
#include <AST/VarRefNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::type_named;

namespace
{
    // the two storage classes side by side, so each case names the one it means. no methods and no
    // constructor of their own: what is under test is the declaration this owner mints, not anything
    // the parser does with it
    const char *k_types =
        "class Handle {\n"
        "    int32 $value;\n"
        "}\n"
        "struct Point {\n"
        "    int32 $x;\n"
        "}\n";

    // the self type as a constructor holds it: one TypeNode, shared between the declaration and the
    // return type, exactly as Parser::parse_constructor hands over ctor_decl->return_type
    TypeNode &self_type_node(Module &m, const std::string &name)
    {
        auto *decl = type_named(m, name);
        REQUIRE(decl != nullptr);

        return m.nodes.emplace_back<TypeNode>(decl->value_type());
    }
}

// the assertion the synthesized copy constructor was missing: it built its `$this` by hand and gave a
// class none of this, and nothing anywhere said so - the only thing keeping that harmless was
// copy_is_synthesizable's is_struct() guard, which lives in another file entirely
TEST_CASE("a class constructor's $this carries its allocation", "[constructor]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_types);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    TypeNode &self = self_type_node(m, "Handle");

    VarDeclNode &this_decl = declare_constructor_this(m, self, *type_named(m, "Handle")->name_token);

    REQUIRE(this_decl.name_full() == "$this");

    // the value type, not the borrow a method's receiver is - the returned-local move in
    // AST::OwnershipPass rests on it
    REQUIRE(this_decl.type() == type_named(m, "Handle")->value_type());
    REQUIRE_FALSE(this_decl.type().is_pointer());

    // and the heap block, in the initializer, because an initializer runs where it was written
    REQUIRE(this_decl.init_expr != nullptr);
    REQUIRE(this_decl.init_expr->get_node_type() == NodeType::n_expr_class_alloc);
    REQUIRE(this_decl.init_expr->result_type() == type_named(m, "Handle")->value_type());
}

TEST_CASE("a struct constructor's $this has no initializer", "[constructor]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_types);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    TypeNode &self = self_type_node(m, "Point");

    VarDeclNode &this_decl = declare_constructor_this(m, self, *type_named(m, "Point")->name_token);

    REQUIRE(this_decl.name_full() == "$this");
    REQUIRE(this_decl.type() == type_named(m, "Point")->value_type());

    // its slot is already there when the frame is entered, and StmtCodegen::ensure_var_slot zero-fills
    // it - there is nothing to make
    REQUIRE(this_decl.init_expr == nullptr);
}

TEST_CASE("a constructor body ends in one return $this", "[constructor]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_types);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    TypeNode &self = self_type_node(m, "Point");
    const TokenReference &at = *type_named(m, "Point")->name_token;

    VarDeclNode &this_decl = declare_constructor_this(m, self, at);

    auto &decl = m.nodes.emplace_back<FunctionDeclNode>(at);
    decl.body = &m.nodes.emplace_back<ScopeNode>();
    decl.body->add_vardecl(this_decl);

    close_constructor_body(m, decl, this_decl);

    REQUIRE(decl.body->children.size() == 2);
    REQUIRE(decl.body->children.back().has_type<ReturnNode>());

    // reading the declaration it was given, through a node of its own
    auto &ret = decl.body->children.back().get<ReturnNode>();
    REQUIRE(ret.expr != nullptr);
    REQUIRE(ret.expr->get_node_type() == NodeType::n_varref);
    REQUIRE(&static_cast<VarRefNode *>(ret.expr)->get_var().decl() == &this_decl);

    // and asked again it adds nothing: a body that already leaves on every path is owed no return, and
    // one appended behind the first is dead code the ownership pass still hangs an unwind on
    close_constructor_body(m, decl, this_decl);
    REQUIRE(decl.body->children.size() == 2);
}
