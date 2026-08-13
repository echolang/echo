#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTConstness.h>
#include <AST/ASTInstantiation.h>
#include <AST/ASTMemberLookup.h>
#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ScopeNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/TypeNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::calls_to;
using EchoTests::decls_named;
using EchoTests::has_issue_containing;
using EchoTests::type_named;

TEST_CASE("a static is registered on its type, in neither the namespace nor the method table", "[statics]")
{
    // **three tables and a static is in exactly one of them.** absent from the namespace overload set
    // like a method, and absent from the *method* table unlike one - because a method is reached
    // through a receiver and a static declares no args[0] to be that receiver. sharing the table would
    // make `$p->origin()` resolve to a declaration whose first parameter the caller never wrote
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    static function origin() : Point { return Point(0); }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *point = type_named(m, "Point");
    REQUIRE(point != nullptr);

    auto statics = find_static_functions(&point->complex_type(), "origin");
    REQUIRE(statics.size() == 1);
    REQUIRE(statics[0]->owner_type == &point->complex_type());

    // not in the method table, so `$p->origin()` finds nothing
    REQUIRE(find_member_functions(&point->complex_type(), "origin").empty());

    // and nowhere in the namespace, so a bare `origin()` finds nothing
    REQUIRE(bundle->collector.functions.overloads("origin", *point->ast_namespace).empty());
}

TEST_CASE("a static has an owner and no receiver, and those are two questions", "[statics]")
{
    // is_member() answers "does a type own this" - which a static does, and has to: the owner is the
    // only thing separating one type's `make` from another's in the mangled name, and the only way
    // AST::enclosing_type_of can let it reach its own type's private members.
    //
    // has_receiver() answers "did the caller not write args[0]" - which for a static is false, and
    // every offset, drop and const-receiver rule is taken against *that*
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    static function of(int32 $v) : Point { return Point($v); }\n"
        "    function get() : int32 { return $this->x; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *point = type_named(m, "Point");
    REQUIRE(point != nullptr);

    auto *of = find_static_functions(&point->complex_type(), "of").at(0);
    auto *get = find_member_functions(&point->complex_type(), "get").at(0);

    REQUIRE(of->is_member());
    REQUIRE_FALSE(of->has_receiver());
    REQUIRE(of->implicit_arg_count() == 0);

    // so args[0] is the parameter the user wrote, and it is counted as argument 1
    REQUIRE(of->args.size() == 1);
    REQUIRE(of->args[0]->name() == "v");

    // and nothing reads it as a receiver: a static is never const-qualified
    REQUIRE_FALSE(receiver_is_const(*of));

    // the method beside it is unchanged in both answers
    REQUIRE(get->is_member());
    REQUIRE(get->has_receiver());
    REQUIRE(get->implicit_arg_count() == 1);
}

TEST_CASE("a static and a method of one name do not collide", "[statics]")
{
    // they are told apart at every call site - `Point::square()` and `$p->square()` - so they are two
    // declarations rather than a duplicate signature. that is why register_static_function checks the
    // static list alone rather than across both
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point {\n"
        "    int32 $x;\n"
        "    static function square() : int32 { return 4; }\n"
        "    function square() : int32 { return $this->x; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *point = type_named(m, "Point");
    REQUIRE(point != nullptr);

    REQUIRE(find_static_functions(&point->complex_type(), "square").size() == 1);
    REQUIRE(find_member_functions(&point->complex_type(), "square").size() == 1);
}

TEST_CASE("a static's owner binds the owner's type parameters", "[statics][generics]")
{
    // **the seed AST::can_instantiate cannot do without.** a method binds `result<T, E>`'s parameters
    // by unifying args[0]'s `result<T, E>&` against the receiver. a static has no args[0], and E
    // appears nowhere in `ok`'s signature - so the call site's owner is the only thing that can say
    // what E is, and without it the instantiation is undecidable and reported by nobody
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct result<T, E> {\n"
        "    T $value;\n"
        "    E $error;\n"
        "    static function ok(T $v, E $blank) : result<T, E> {\n"
        "        return result<T, E>($v, $blank);\n"
        "    }\n"
        "}\n"
        "$a = result<int32, bool>::ok(1, false);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *result_type = type_named(m, "result");
    REQUIRE(result_type != nullptr);

    auto *tmpl = find_static_functions(&result_type->complex_type(), "ok").at(0);

    // the owner carries both of its parameters ahead of the static's own, exactly as a method's does
    REQUIRE(tmpl->inherited_type_param_count == 2);

    // the call resolved to an instance, which is only possible if E bound - and E could only have
    // bound from the owner
    auto calls = calls_to(m, "ok");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->decl != nullptr);
    REQUIRE_FALSE(calls[0]->decl->is_generic());
    REQUIRE(calls[0]->decl->instantiation_args.size() == 2);
    REQUIRE(calls[0]->decl->instantiation_args[1].is_boolean_type());
}

TEST_CASE("static is refused where no type owns the declaration", "[statics]")
{
    EchoTests::assert_code_emits_issue(
        "static function nope() : int32 { return 1; }\n",
        "'nope' is not declared inside a type, so it cannot be static"
    );
}

TEST_CASE("const is refused beside static, once", "[statics]")
{
    // one diagnostic and not two: the `const` is dropped where the pair is refused, so the
    // "not a method, so it cannot be const" rule beside it never sees it
    EchoTests::assert_code_emits_issue(
        "struct Point {\n"
        "    int32 $x;\n"
        "    const static function origin() : Point { return Point(0); }\n"
        "}\n",
        "'origin' is static, so it has no receiver for 'const' to qualify"
    );
}
