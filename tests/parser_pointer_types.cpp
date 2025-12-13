#include <catch2/catch_test_macros.hpp>

#include <AST/FunctionDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

// the `ptr<T>` / `T&` type grammar. `ptr<...>` recurses into a full type rather than being a
// flag with hardcoded angle brackets, which is what makes nesting and generic pointees work

namespace
{
    // the declared type of the named variable in the test module, or unknown when absent
    AST::ValueType decl_type(AST::Bundle &bundle, const std::string &varname)
    {
        auto &module = bundle.modules.find_module("test");
        for (auto *decl : module.nodes.of_type<AST::VarDeclNode>()) {
            if (decl->name_full() == varname && decl->has_type()) {
                return decl->type();
            }
        }
        return AST::ValueType::make_unknown();
    }

    AST::FunctionDeclNode *find_function(AST::Bundle &bundle, const std::string &name)
    {
        auto &module = bundle.modules.find_module("test");
        for (auto *func : module.nodes.of_type<AST::FunctionDeclNode>()) {
            if (func->func_name() == name) {
                return func;
            }
        }
        return nullptr;
    }
}

TEST_CASE( "ptr<ptr<T>> parses, closing on a split >>", "[parser][pointer]" )
{
    auto bundle = EchoTests::tests_make_parsed_bundle("ptr<ptr<int>> $pp;");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto type = decl_type(*bundle, "$pp");
    REQUIRE(type.is_pointer());
    REQUIRE(type.pointee().is_pointer());
    REQUIRE(type.pointee().pointee().is_primitive_of_type(AST::ValueTypePrimitive::t_int32));
}

TEST_CASE( "A generic application can be a pointee", "[parser][pointer][generics]" )
{
    // the old parser owned the closing `>` of `ptr<`, so it explicitly refused to parse a
    // generic application inside one
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        struct Box<T> {
            T $item;
        }
        ptr<Box<int>> $p;
    )");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto type = decl_type(*bundle, "$p");
    REQUIRE(type.is_pointer());
    REQUIRE(type.pointee().is_struct());
    REQUIRE(type.pointee().get_type_desciption() == "Box<int32>");
}

TEST_CASE( "A borrow is spelled with a trailing &", "[parser][pointer]" )
{
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        $var = 10;
        int& $r = &$var;
    )");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto type = decl_type(*bundle, "$r");
    REQUIRE(type.is_pointer());
    REQUIRE_FALSE(type.is_nullable());
    REQUIRE(type.pointee().is_primitive_of_type(AST::ValueTypePrimitive::t_int32));
}

TEST_CASE( "A space before & does not lose the reference", "[parser][pointer]" )
{
    // the lexer only emits t_ref when the & abuts a name character, so `int &$x` and
    // `int & $x` arrive as different tokens. in type position both mean a borrow
    auto tight = EchoTests::tests_make_parsed_bundle("function a(int &$x) : void {}");
    auto loose = EchoTests::tests_make_parsed_bundle("function a(int & $x) : void {}");

    REQUIRE_FALSE(tight->collector.has_critical_issues());
    REQUIRE_FALSE(loose->collector.has_critical_issues());

    auto *tight_fn = find_function(*tight, "a");
    auto *loose_fn = find_function(*loose, "a");
    REQUIRE(tight_fn != nullptr);
    REQUIRE(loose_fn != nullptr);

    REQUIRE(tight_fn->args[0]->type().is_pointer());
    REQUIRE(loose_fn->args[0]->type().is_pointer());
    REQUIRE(tight_fn->args[0]->type() == loose_fn->args[0]->type());
}

TEST_CASE( "A borrow is a legal return type", "[parser][pointer]" )
{
    // `T&` used to be handled by the var-decl parser rather than the type parser, so it was
    // only recognised where a variable was being declared
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        function pick(int &$x) : int& {
            return $x;
        }
    )");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *fn = find_function(*bundle, "pick");
    REQUIRE(fn != nullptr);
    REQUIRE(fn->get_return_type().is_pointer());
    REQUIRE(fn->get_return_type().get_type_desciption() == "int32&");
}

TEST_CASE( "A pointer type renders once in the AST dump", "[parser][pointer]" )
{
    // TypeNode used to carry its own const/pointer bools alongside the ValueType and render
    // both, so `const int $x` printed as `type<const const int32>`
    REQUIRE_NODE_DESC("const int $x = 5;", "vardecl<type<const int32>>($x) = literal<int32>(5)");
}

TEST_CASE( "A borrow is a legal struct property", "[parser][pointer]" )
{
    // the struct body kept its own hand-rolled list of "what starts a property declaration" and
    // no entry in it allowed for the `&` suffix, so a borrow property was silently not a property
    // at all - the declaration fell through to "Unexpected token 'identifier'". both dispatch
    // sites now ask Parser::starts_vardecl rather than keeping a private copy
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        struct Holder {
            int& $target;
        }
    )");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto type = decl_type(*bundle, "$target");
    REQUIRE(type.is_pointer());
    REQUIRE_FALSE(type.is_nullable());
    REQUIRE(type.pointee().is_primitive_of_type(AST::ValueTypePrimitive::t_int32));
}

TEST_CASE( "A borrow property accepts the spaced spelling too", "[parser][pointer]" )
{
    auto bundle = EchoTests::tests_make_parsed_bundle("struct Holder { int & $target; }");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    REQUIRE(decl_type(*bundle, "$target").is_pointer());
}

TEST_CASE( "A borrow is a legal generic argument", "[parser][pointer][generics]" )
{
    // spelled through `ptr<...>` because a bare `Box<int> $b;` is not recognised as a statement
    // at all - the scope dispatch has no entry for a generic application, independent of
    // pointers. what is under test here is that `&` survives inside a type argument list, where
    // the closing `>` and the reference suffix have to be told apart
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        struct Box<T> {
            T $item;
        }
        ptr<Box<int&>> $b;
    )");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto type = decl_type(*bundle, "$b");
    REQUIRE(type.is_pointer());
    REQUIRE(type.pointee().is_struct());
    REQUIRE(type.pointee().get_type_desciption() == "Box<int32&>");

    // the borrow reached the instantiated property, so the instance really is parameterised on
    // the pointer type rather than having quietly decayed to Box<int32>
    REQUIRE(type.pointee().get_complex_type()->get_property_type("item").is_pointer());
}

TEST_CASE( "A reference cannot be taken twice", "[parser][pointer]" )
{
    // note the spacing: `int&&` lexes as the single logical-and token, so the type parser never
    // sees two references and the dedicated diagnostic only fires on the spaced spelling. both
    // spellings are rejected, just by different parts of the compiler
    EchoTests::assert_code_emits_issue(
        "function f(int& & $x) : void {}",
        "A reference cannot be taken twice, write 'ptr<int32&>' instead");

    auto glued = EchoTests::tests_make_parsed_bundle("function f(int&& $x) : void {}");
    REQUIRE(glued->collector.has_critical_issues());
}

TEST_CASE( "':$:$' is an address-of, and a third has nothing left to reach", "[parser][pointer]" )
{
    // `$out:$:$` is not two markers - the parser collapses it to `&$out`, which is what makes the
    // identity in the doc hold without a special case in the adjustment pass
    // (book/concept/pointers_and_refs_v2.md, "Pointers to pointers")
    auto bundle = EchoTests::tests_make_parsed_bundle(R"(
        $a = 1;
        ptr<int> $p = &$a;
        ptr<ptr<int>> $pp = &$p;
        $q = $pp:$:$;
    )");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    // the address of $pp's own slot, so one level deeper than $pp itself
    REQUIRE(decl_type(*bundle, "$q").get_type_desciption() == "ptr<ptr<int32>>&");

    // an address-of is not a place, so a further `:$` has nothing to peel
    EchoTests::assert_code_emits_issue(R"(
        $a = 1;
        ptr<int> $p = &$a;
        ptr<ptr<int>> $pp = &$p;
        $q = $pp:$:$:$;
    )", "':$' needs an expression with storage to reach the pointer of");
}

TEST_CASE( "A borrow cast accepts both '&' spellings", "[parser][pointer]" )
{
    // the narrowing `ptr<T>` -> `T&` asserts non-nullness, so it has to be written out. the cast
    // production keys on `identifier & (`, and the lexer's t_ref/t_and split means both spacings
    // have to be accepted or the spaced one parses as a bitwise and
    //
    // it is also the promotion AST::narrowing_promotes_raw_storage names, hence the block - which is
    // orthogonal to the spelling question and has to be written the same way on both sides of it
    auto tight = EchoTests::tests_make_parsed_bundle(R"(
        $a = 5;
        ptr<int> $p = &$a;
        unsafe { int& $r = int&($p:$); }
    )");
    REQUIRE_FALSE(tight->collector.has_critical_issues());

    auto loose = EchoTests::tests_make_parsed_bundle(R"(
        $a = 5;
        ptr<int> $p = &$a;
        unsafe { int& $r = int & ($p:$); }
    )");
    REQUIRE_FALSE(loose->collector.has_critical_issues());

    REQUIRE(decl_type(*tight, "$r") == decl_type(*loose, "$r"));
}
