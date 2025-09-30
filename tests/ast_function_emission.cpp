#include <catch2/catch_test_macros.hpp>

#include "helpers.h"

#include <AST/ASTFunctionEmission.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ASTModule.h>

// AST::function_emission_kind is the sole owner of "does this declaration have a symbol, does this compiler
// emit its body, and where does the definition go". Three readers derive from it - both loops of
// TypeLowering::build_function_maps and StmtCodegen::gen_function_decl - so an arm answered wrongly here is a
// duplicate symbol, a missing symbol, or a body emitted into the wrong object file.
//
// the arms overlap on purpose, which is why they are pinned one at a time: a declaration can be generic *and*
// an intrinsic, `#[inline]` *and* an intrinsic, instantiated *and* a builtin. What the order resolves is
// which fact wins.

namespace
{

// the one declaration of this name, as a bundle keeps it. Fails the test rather than returning null, because
// every case below is written against a name it just declared
AST::FunctionDeclNode *only_decl(AST::Bundle &bundle, const std::string &name)
{
    auto &module = bundle.modules.find_module("test");
    auto found = EchoTests::decls_named(module, name);

    REQUIRE(found.size() >= 1);
    return found.front();
}

};

TEST_CASE("an ordinary function is defined once by its own module", "[emission]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function plain(int32 $n) : int32 { return $n; }");

    REQUIRE(AST::function_emission_kind(only_decl(*bundle, "plain"))
        == AST::FunctionEmission::t_module_local);
}

TEST_CASE("#[inline] asks for the body to be copied into every calling unit", "[emission][inline]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[inline]\n"
        "function hot(int32 $n) : int32 { return $n; }");

    AST::FunctionDeclNode *decl = only_decl(*bundle, "hot");

    REQUIRE(decl->is_inline);

    // the same answer a generic instance gets, which is the whole point: "copy me to the caller" is one
    // mechanism with two ways of asking for it
    REQUIRE(AST::function_emission_kind(decl) == AST::FunctionEmission::t_odr_shared);
}

TEST_CASE("#[inline] does not invent a body for a declaration that has none", "[emission][inline]")
{
    // stdlib/math/intrinsics.eco marks every intrinsic `#[inline]`, so this combination is not hypothetical.
    // The intrinsic arm must win: there is no body of ours to copy anywhere, and answering t_odr_shared would
    // have StmtCodegen try to emit one
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[inline]\n"
        "#[intrinsic: \"llvm.sqrt\"]\n"
        "function rooted(float $x) : float;");

    AST::FunctionDeclNode *decl = only_decl(*bundle, "rooted");

    REQUIRE(decl->is_inline);
    REQUIRE(AST::function_emission_kind(decl) == AST::FunctionEmission::t_intrinsic);
}

TEST_CASE("a generic template has no symbol, and its instance is ODR-shared", "[emission][generics]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function identity<T>(T $value) : T { return $value; }\n"
        "identity<int32>(7);");

    auto &module = bundle->modules.find_module("test");
    auto declarations = EchoTests::decls_named(module, "identity");

    // the template and the instance the monomorphizer cloned from it
    REQUIRE(declarations.size() == 2);

    bool saw_template = false;
    bool saw_instance = false;

    for (AST::FunctionDeclNode *decl : declarations) {
        if (decl->is_generic()) {
            // no concrete signature to mangle, so nothing may declare or define a symbol for it
            REQUIRE(AST::function_emission_kind(decl) == AST::FunctionEmission::t_no_symbol);
            saw_template = true;
            continue;
        }

        REQUIRE(decl->is_instantiated());

        // **the invariant a module object cache rests on.** Two consumers may each instantiate this template,
        // and each emits its own definition - so the linkage has to tolerate duplicates rather than the
        // template's own module having to guess which instances somebody will one day want
        REQUIRE(AST::function_emission_kind(decl) == AST::FunctionEmission::t_odr_shared);
        saw_instance = true;
    }

    REQUIRE(saw_template);
    REQUIRE(saw_instance);
}

TEST_CASE("an extern names a symbol it never defines", "[emission]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "extern { function some_c_thing(int32 $n) : int32; }");

    const AST::FunctionEmission kind = AST::function_emission_kind(only_decl(*bundle, "some_c_thing"));

    REQUIRE(kind == AST::FunctionEmission::t_extern_symbol);

    // declared eagerly, defined never - the asymmetry the two predicates exist to express
    REQUIRE(AST::emission_needs_declaration(kind));
    REQUIRE_FALSE(AST::emission_has_body(kind));
}

TEST_CASE("an unresolved call has no symbol rather than a plausible one", "[emission]")
{
    // a null decl is a call nothing resolved. Answering anything else would turn a resolution failure into a
    // link failure, which is a worse diagnostic much further from the mistake
    REQUIRE(AST::function_emission_kind(nullptr) == AST::FunctionEmission::t_no_symbol);
}

TEST_CASE("the emission predicates agree with each other", "[emission]")
{
    // the two questions are asked in different places and must not disagree: a body can only be emitted
    // against a symbol, so anything with one needs a declaration to define against
    const AST::FunctionEmission kinds[] = {
        AST::FunctionEmission::t_no_symbol,
        AST::FunctionEmission::t_extern_symbol,
        AST::FunctionEmission::t_intrinsic,
        AST::FunctionEmission::t_module_local,
        AST::FunctionEmission::t_odr_shared,
    };

    for (const AST::FunctionEmission kind : kinds) {
        if (AST::emission_has_body(kind)) {
            REQUIRE(AST::emission_needs_declaration(kind));
        }
    }
}
