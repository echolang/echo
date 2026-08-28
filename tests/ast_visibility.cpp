#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTConstness.h>
#include <AST/ASTValueType.h>
#include <AST/ASTVisibility.h>
#include <AST/ConstDeclNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::decls_named;
using EchoTests::type_named;

// **the default is the module, and one keyword covers two axes.** the e2e corpus holds what a program is
// refused for; what these cases hold is the thing underneath it that no diagnostic shows - which level each
// declaration ended up on, and that the *position* is what narrowed a `private`
//
// the partition is the part worth pinning here rather than through a diagnostic: `public const int32 $x;`,
// `public const MAX = 5;` and `public const function f()` are three different declarations, told apart by
// four predicates that all scan from the head of the statement. so the modifier has to be consumed before
// any of them is asked, and a case that only checked the *level* would pass while the statement was
// classified as the wrong kind entirely

TEST_CASE("a declaration with no modifier belongs to its module", "[visibility]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Plain { int32 $n; }\n"
        "function plain() : int32 { return 1; }\n"
        "const int32 PLAIN = 1;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    REQUIRE(type_named(m, "Plain")->complex_type().visibility == Visibility::t_module);

    auto plain = decls_named(m, "plain");
    REQUIRE(plain.size() == 1);
    REQUIRE(plain[0]->visibility == Visibility::t_module);

    const auto constants = m.nodes.of_type<ConstDeclNode>();
    REQUIRE(constants.size() == 1);
    REQUIRE(constants[0]->visibility == Visibility::t_module);
}

TEST_CASE("the three levels land on the three rungs", "[visibility]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "private struct Hidden { int32 $n; }\n"
        "internal struct Shared { int32 $n; }\n"
        "public struct Offered { int32 $n; }\n"
        "private function hidden() : int32 { return 1; }\n"
        "internal function shared() : int32 { return 2; }\n"
        "public function offered() : int32 { return 3; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    REQUIRE(type_named(m, "Hidden")->complex_type().visibility == Visibility::t_file);
    REQUIRE(type_named(m, "Shared")->complex_type().visibility == Visibility::t_module);
    REQUIRE(type_named(m, "Offered")->complex_type().visibility == Visibility::t_public);

    REQUIRE(decls_named(m, "hidden")[0]->visibility == Visibility::t_file);
    REQUIRE(decls_named(m, "shared")[0]->visibility == Visibility::t_module);
    REQUIRE(decls_named(m, "offered")[0]->visibility == Visibility::t_public);
}

TEST_CASE("a private member is the owner axis, not the file", "[visibility]")
{
    // the same word on a member and on a free declaration, in one program. `t_owner` versus `t_file` is the
    // whole of what the position decided, and nothing downstream re-asks it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "private function free_one() : int32 { return 1; }\n"
        "struct Box\n"
        "{\n"
        "    private int32 $hidden;\n"
        "    int32 $shown;\n"
        "    private function secret() : int32 { return $this->hidden; }\n"
        "    function open_one() : int32 { return $this->secret(); }\n"
        "    constructor() { $this->hidden = 0; $this->shown = 0; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    REQUIRE(decls_named(m, "free_one")[0]->visibility == Visibility::t_file);
    REQUIRE(decls_named(m, "secret")[0]->visibility == Visibility::t_owner);
    REQUIRE(decls_named(m, "open_one")[0]->visibility == Visibility::t_public);

    auto *box = type_named(m, "Box");
    REQUIRE(box != nullptr);

    // the layout-side copy, which is the only thing an instantiation can answer with
    const ComplexType::Property *hidden = box->complex_type().find_property("hidden");
    const ComplexType::Property *shown = box->complex_type().find_property("shown");
    REQUIRE(hidden != nullptr);
    REQUIRE(shown != nullptr);
    REQUIRE(hidden->is_private());
    REQUIRE_FALSE(shown->is_private());
    REQUIRE(hidden->visibility == Visibility::t_owner);
    REQUIRE(shown->visibility == Visibility::t_public);
}

TEST_CASE("a modifier does not reclassify the declaration behind it", "[visibility]")
{
    // **the case the whole design rests on.** all three of these start `public const`, and only the token
    // after it says which kind of declaration follows: a `$` name is a property, a bare name is a constant,
    // and `function` is a method whose *receiver* is const. if the modifier were read inside one of the
    // dispatch arms rather than ahead of all of them, one of these three would be claimed by the wrong arm -
    // which is exactly how `private function` used to fail
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Reading\n"
        "{\n"
        "    public const int32 $limit;\n"
        "    public const int32 STEP = 2;\n"
        "    public const function limit() : int32 { return $this->limit; }\n"
        "    constructor(int32 $limit) { $this->limit = $limit; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *reading = type_named(m, "Reading");
    REQUIRE(reading != nullptr);

    // the property, whose *type* is const - which is the whole of what `public const` on one means
    const ComplexType::Property *limit = reading->complex_type().find_property("limit");
    REQUIRE(limit != nullptr);
    REQUIRE(limit->type.is_const());
    REQUIRE_FALSE(limit->is_private());
    REQUIRE(limit->visibility == Visibility::t_public);

    // the constant, which is a member of the type and so has no level of its own to narrow
    const auto constants = m.nodes.of_type<ConstDeclNode>();
    REQUIRE(constants.size() == 1);
    REQUIRE(constants[0]->name() == "STEP");
    REQUIRE(constants[0]->visibility == Visibility::t_public);

    // and the method, where the `const` went to the receiver rather than to a visibility
    auto methods = decls_named(m, "limit");
    REQUIRE(methods.size() == 1);
    REQUIRE(methods[0]->visibility == Visibility::t_public);
    REQUIRE(receiver_is_const(*methods[0]));
}

TEST_CASE("visible_from answers the file and module axes and declines the owner one", "[visibility]")
{
    // the rule on its own, away from any program: two origins and a level, which is all it takes. the
    // `t_owner` arm answering *true* is deliberate - that axis is about types, so a caller that has not
    // split the two arms must not silently refuse every private member in the program
    Module lib("lib", 1);
    Module app("app", 2);

    File a("a.eco");
    File b("b.eco");

    const DeclarationOrigin in_a { &lib, &a };
    const DeclarationOrigin in_b { &lib, &b };
    const DeclarationOrigin in_app { &app, nullptr };
    const DeclarationOrigin nowhere {};

    REQUIRE(visible_from(Visibility::t_public, in_a, in_app));

    REQUIRE(visible_from(Visibility::t_module, in_a, in_b));
    REQUIRE_FALSE(visible_from(Visibility::t_module, in_a, in_app));

    REQUIRE(visible_from(Visibility::t_file, in_a, in_a));
    REQUIRE_FALSE(visible_from(Visibility::t_file, in_a, in_b));

    REQUIRE(visible_from(Visibility::t_owner, in_a, in_app));

    // an unknown origin on either side reaches everywhere. that is what exempts a generic instantiation's
    // body, and what keeps a compiler-minted declaration out of a refusal it could never satisfy
    REQUIRE(visible_from(Visibility::t_module, nowhere, in_app));
    REQUIRE(visible_from(Visibility::t_module, in_a, nowhere));
    REQUIRE(visible_from(Visibility::t_file, nowhere, in_app));
}

TEST_CASE("a refusal is worded only where there is something to refuse", "[visibility]")
{
    // the AST::const_receiver_refused / const_receiver_refusal split, and for its reason: the sites that ask
    // the *question* ask it while they still have candidates to weigh, and building a sentence per member
    // call in the program to test it for emptiness is what that split avoids
    Module lib("lib", 1);
    Module app("app", 2);
    File a("a.eco");

    const DeclarationOrigin in_a { &lib, &a };
    const DeclarationOrigin in_app { &app, nullptr };

    REQUIRE(visibility_refusal(Visibility::t_public, in_a, in_app, "thing()").empty());
    REQUIRE(visibility_refusal(Visibility::t_owner, in_a, in_app, "thing()").empty());
    REQUIRE(visibility_refusal(Visibility::t_module, in_a, in_a, "thing()").empty());

    const std::string module_refusal = visibility_refusal(Visibility::t_module, in_a, in_app, "thing()");
    REQUIRE_FALSE(module_refusal.empty());
    REQUIRE(module_refusal.find("internal to the module 'lib'") != std::string::npos);
    REQUIRE(module_refusal.find("Write 'public'") != std::string::npos);

    const std::string file_refusal = visibility_refusal(Visibility::t_file, in_a, in_app, "thing()");
    REQUIRE_FALSE(file_refusal.empty());
    REQUIRE(file_refusal.find("private to 'a.eco'") != std::string::npos);
}

TEST_CASE("internal on a member is the module axis, not a refusal", "[visibility]")
{
    // the same word on a member and on a free declaration, in one program. `t_module` versus
    // `t_module` is the whole of what the position decided this time: a member now sits on the
    // module axis the top level already had, and nothing downstream re-asks "is this a member"
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "internal function free_one() : int32 { return 1; }\n"
        "struct Box\n"
        "{\n"
        "    internal int32 $shared;\n"
        "    int32 $shown;\n"
        "    internal function hidden() : int32 { return $this->shared; }\n"
        "    function open_one() : int32 { return $this->hidden(); }\n"
        "    constructor() { $this->shared = 0; $this->shown = 0; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    REQUIRE(decls_named(m, "free_one")[0]->visibility == Visibility::t_module);
    REQUIRE(decls_named(m, "hidden")[0]->visibility == Visibility::t_module);
    REQUIRE(decls_named(m, "open_one")[0]->visibility == Visibility::t_public);

    auto *box = type_named(m, "Box");
    REQUIRE(box != nullptr);

    const ComplexType::Property *shared = box->complex_type().find_property("shared");
    const ComplexType::Property *shown = box->complex_type().find_property("shown");
    REQUIRE(shared != nullptr);
    REQUIRE(shown != nullptr);
    REQUIRE(shared->visibility == Visibility::t_module);
    REQUIRE_FALSE(shared->is_private());
    REQUIRE(shown->visibility == Visibility::t_public);
}

TEST_CASE("member_visibility maps internal to the module rung", "[visibility]")
{
    REQUIRE(member_visibility(Visibility::t_module) == Visibility::t_module);
    REQUIRE(member_visibility(Visibility::t_file) == Visibility::t_owner);
    REQUIRE(member_visibility(Visibility::t_public) == Visibility::t_public);
    REQUIRE(member_visibility(std::nullopt) == Visibility::t_public);
}

TEST_CASE("an internal property does not suppress the field-wise constructor", "[visibility]")
{
    auto internal_fields = EchoTests::tests_make_parsed_bundle(
        "struct Open\n"
        "{\n"
        "    internal int32 $n;\n"
        "    int32 $m;\n"
        "}\n");

    REQUIRE_FALSE(internal_fields->collector.has_critical_issues());

    auto *open = type_named(internal_fields->modules.find_module("test"), "Open");
    REQUIRE(open != nullptr);
    REQUIRE(open->synthesized_constructor() != nullptr);
    REQUIRE(open->synthesized_constructor()->args.size() == 2);

    auto private_field = EchoTests::tests_make_parsed_bundle(
        "struct Shut\n"
        "{\n"
        "    private int32 $n;\n"
        "    int32 $m;\n"
        "}\n");

    REQUIRE_FALSE(private_field->collector.has_critical_issues());

    auto *shut = type_named(private_field->modules.find_module("test"), "Shut");
    REQUIRE(shut != nullptr);
    REQUIRE(shut->synthesized_constructor() == nullptr);
}

TEST_CASE("an instantiation copies a property's module visibility", "[visibility]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T>\n"
        "{\n"
        "    internal T $item;\n"
        "}\n"
        "function take(Box<int32> $b) : int32 { return 1; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto *box = type_named(bundle->modules.find_module("test"), "Box");
    REQUIRE(box != nullptr);

    const ValueType i32 = EchoTests::prim(ValueTypePrimitive::t_int32);
    ComplexType *of_int = bundle->collector.type_registry.get_or_create_instantiation(
        &box->complex_type(), { i32 });
    REQUIRE(of_int != nullptr);

    const ComplexType::Property *item = of_int->find_property("item");
    REQUIRE(item != nullptr);
    REQUIRE(item->visibility == Visibility::t_module);
    REQUIRE_FALSE(item->is_private());
}

TEST_CASE("a neighbour in the same module can name an internal member", "[visibility]")
{
    const std::vector<std::string> files = {
        "struct Node\n"
        "{\n"
        "    internal int32 $kind;\n"
        "    internal const function hidden() : int32 { return $this->kind; }\n"
        "}\n",
        "function write(const Node& $n) : int32\n"
        "{\n"
        "    return $n->kind + $n->hidden();\n"
        "}\n"
        "function main() : int32 { return write(Node(1)); }\n",
    };
    auto bundle = EchoTests::tests_make_parsed_bundle(files);

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}
