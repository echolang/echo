#include <catch2/catch_test_macros.hpp>

#include "helpers.h"

#include <AST/ASTBundle.h>
#include <AST/ASTIssue.h>

#include <string>

// AST::PointerAdjuster - the pass that writes every auto-deref into the tree explicitly.
//
// a pointer "behaves like the value it points to", and codegen honoured that by emitting a second
// load. the AST did not, so a read of `int32& $x` claimed int32& while the value produced was an
// int32. this pass reconciles the two, and after it result_type() means what it says.
//
// the pass is invisible in `echoc run -a`, which prints before it runs, so these descriptions are
// the only place its output is observable. tests_make_parsed_bundle runs the real pipeline
// (monomorphize -> adjust -> type check), so what is asserted here is what codegen receives.
//
// the vocabulary, from src/AST/ExprNode.cpp: deref<T>(...) is the inserted auto-deref,
// addrof<T&>(...) an address-of, index<T>(base[i]) an element, and peel<T>(...) the `:$` marker -
// which must never survive, since codegen throws on one

namespace {
    std::string desc(const std::string &code)
    {
        return EchoTests::tests_make_node_description(code);
    }

    bool contains(const std::string &haystack, const std::string &needle)
    {
        return haystack.find(needle) != std::string::npos;
    }
}

TEST_CASE("A pointer read in value position gains exactly one deref", "[sema][pointer]")
{
    auto d = desc("$var = 10;\nint& $r = &$var;\necho $r;\n");

    REQUIRE(contains(d, "call echo(deref<int32>(varref<int32&>(var($r)))): void"));

    // exactly one - a second would read through the int32 the first produced
    REQUIRE_FALSE(contains(d, "deref<int32>(deref<"));
}

TEST_CASE("A non-pointer read gains no deref", "[sema][pointer]")
{
    auto d = desc("$var = 10;\necho $var;\n");

    REQUIRE(contains(d, "call echo(varref<int32>(var($var))): void"));
    REQUIRE_FALSE(contains(d, "deref<"));
}

TEST_CASE("':$' is the absence of the deref, and the marker is erased", "[sema][pointer]")
{
    // `:$` compiles to nothing: PointerValueNode exists only to stop the insertion here, and the
    // pass then drops it. one surviving into codegen is a compiler bug, which the visitor there
    // reports as "':$' survived the pointer adjustment pass"
    auto d = desc("$var = 10;\nint& $r = &$var;\nptr<int> $a = $r:$;\n");

    REQUIRE(contains(d, "vardecl<type<ptr<int32>>>($a) = varref<int32&>(var($r))"));
    REQUIRE_FALSE(contains(d, "peel<"));
    REQUIRE_FALSE(contains(d, "deref<"));
}

TEST_CASE("'&' takes the slot's address without reading through it", "[sema][pointer]")
{
    // `&$p` on a ptr<int32> is the address of $p's own slot - a ptr<ptr<int32>> - not the address
    // of what $p points at. an AddrOfExprNode is already the value it means, so no deref is owed
    auto d = desc("$var = 10;\nptr<int> $p = &$var;\nptr<ptr<int>> $pp = &$p;\n");

    REQUIRE(contains(d, "vardecl<type<ptr<ptr<int32>>>>($pp) = addrof<ptr<int32>&>(varref<ptr<int32>>(var($p)))"));
    REQUIRE_FALSE(contains(d, "deref<"));
}

TEST_CASE("A declaration binds a pointer target and reads a value target", "[sema][pointer]")
{
    // as_value_for: a pointer-shaped destination wants the address, anything else reads through.
    // the same rule serves declarations and assignments, which is what keeps binding and
    // write-through from drifting apart
    auto binds = desc("$var = 10;\nint& $r = &$var;\nptr<int> $p = $r:$;\n");
    REQUIRE(contains(binds, "vardecl<type<ptr<int32>>>($p) = varref<int32&>(var($r))"));

    auto reads = desc("$var = 10;\nint& $r = &$var;\nint $copy = $r;\n");
    REQUIRE(contains(reads, "vardecl<type<int32>>($copy) = deref<int32>(varref<int32&>(var($r)))"));
}

TEST_CASE("An assignment writes through, an assignment to ':$' re-seats", "[sema][pointer]")
{
    // the single difference between the two is whether the target carries the deref
    // (book/concept/pointers_and_refs_v2.md, "Binding, writing, and re-seating")
    auto writes = desc("$var = 10;\nptr<int> $p = &$var;\n$p = 20;\n");
    REQUIRE(contains(writes, "deref<int32>(varref<ptr<int32>>(var($p))) = literal<int32>(20)"));

    auto reseats = desc("$a = 1;\n$b = 2;\nptr<int> $p = &$a;\n$p:$ = &$b;\n");
    REQUIRE(contains(reseats, "varref<ptr<int32>>(var($p)) = addrof<int32&>(varref<int32>(var($b)))"));
    REQUIRE_FALSE(contains(reseats, "deref<"));
}

TEST_CASE("A ptr<ptr<T>> assignment keeps the value's address", "[sema][pointer]")
{
    // inside `point_at(ptr<ptr<int>> $out, ...)`, `$out = $target` writes the caller's *pointer*,
    // so the value keeps its address instead of being read through - the target's own type is
    // what decides how far the value is read (tests_eco/pointers/out_parameter.eco)
    auto d = desc(
        "function point_at(ptr<ptr<int>> $out, ptr<int> $target) : void {\n"
        "    $out = $target;\n"
        "}\n");

    REQUIRE(contains(d, "deref<ptr<int32>>(varref<ptr<ptr<int32>>>(var($out))) = varref<ptr<int32>>(var($target))"));
}

TEST_CASE("An index base keeps its address, the index is read", "[sema][pointer]")
{
    // `$p:$[1]` offsets from $p's address; reading through it first would offset from the value
    auto d = desc(
        "struct P { int $a; int $b; }\n"
        "$s = P(1, 2);\n"
        "ptr<int> $p = &$s->a;\n"
        "echo $p:$[1];\n");

    REQUIRE(contains(d, "index<int32>(varref<ptr<int32>>(var($p))[literal<int32>(1)])"));
}

TEST_CASE("A borrowed argument is not wrapped twice", "[sema][pointer]")
{
    // the coercion pass already wrapped the bare `$a` in an address-of; wrapping again would
    // build a ptr<ptr<int32>> out of a borrow parameter
    auto d = desc(
        "function inc(int &$x) : void { $x = $x + 1; }\n"
        "$a = 5;\n"
        "inc($a);\n");

    REQUIRE(contains(d, "call inc(addrof<int32&>(varref<int32>(var($a)))): void"));
    REQUIRE_FALSE(contains(d, "addrof<ptr<int32>&>(addrof<"));
}

TEST_CASE("A pointer argument to a value parameter is read through", "[sema][pointer]")
{
    // where the generic decay lands: an inferred T is not a pointer, so a pointer argument is
    // read to its pointee rather than binding T to the pointer type
    auto d = desc(
        "function id(int $x) : int { return $x; }\n"
        "$a = 5;\n"
        "int& $r = &$a;\n"
        "echo id($r);\n");

    REQUIRE(contains(d, "call id(cast<int32>(deref<int32>(varref<int32&>(var($r)))))"));
}

TEST_CASE("A return fits the declared return type instead of always reading", "[sema][pointer]")
{
    // a `T&` return hands back the address. reading through it here produced an int32 where the
    // signature promised a pointer, which llvm's verifier rejects outright
    auto borrow = desc("function pick(int &$x) : int& { return $x; }\n");
    REQUIRE(contains(borrow, "return(varref<int32&>(var($x)))"));
    REQUIRE_FALSE(contains(borrow, "return(deref<"));

    // a value return still reads through, so the two rules stay distinguishable
    auto value = desc("function read(int &$x) : int { return $x; }\n");
    REQUIRE(contains(value, "return(deref<int32>(varref<int32&>(var($x))))"));
}

TEST_CASE("A condition is a value position", "[sema][pointer]")
{
    auto if_stmt = desc("$a = 1;\nint& $r = &$a;\nif ($r > 0) { echo 1; }\n");
    REQUIRE(contains(if_stmt, "deref<int32>(varref<int32&>(var($r))) > literal<int32>(0)"));

    auto while_stmt = desc("$a = 1;\nint& $r = &$a;\nwhile ($r > 0) { $r = $r - 1; }\n");
    REQUIRE(contains(while_stmt, "deref<int32>(varref<int32&>(var($r))) > literal<int32>(0)"));
}

TEST_CASE("A borrow-typed struct member reads through, and its initializer binds", "[sema][pointer]")
{
    auto d = desc(
        "struct H { int& $t; }\n"
        "$v = 7;\n"
        "$h = H(&$v);\n"
        "echo $h->t;\n");

    // reading the field auto-derefs, like reading any other pointer place
    REQUIRE(contains(d, "call echo(deref<int32>(ma<int32&>(varref<H>(var($h))->t))): void"));

    // but the synthesized constructor *binds* the field. a plain write-through there would store
    // through a field that has never been seated - the initializer is spelled `$this->t:$ = $t`
    // by the struct parser precisely so this deref is absent
    REQUIRE(contains(d, "ma<int32&>(varref<H>(var($this))->t) = varref<int32&>(var(t))"));
    REQUIRE_FALSE(contains(d, "deref<int32>(ma<int32&>(varref<H>(var($this))->t))"));
}

TEST_CASE("A member reached through an element is typed by the field", "[sema][pointer]")
{
    // `$p:$[0]->x` puts an IndexExprNode under the `->`. MemberAccessNode::result_type() used to
    // switch on the base's node class and only knew a varref and a nested member access, so this
    // shape answered void: the write went untyped, the read threw in codegen and no diagnostic
    // fired anywhere in between (todo/B16)
    auto d = desc(
        "struct P { int $x; }\n"
        "$pt = P(1);\n"
        "ptr<P> $p = &$pt;\n"
        "echo $p:$[0]->x;\n");

    REQUIRE(contains(d, "ma<int32>(index<P>("));
    REQUIRE_FALSE(contains(d, "ma<void>"));
}

TEST_CASE("A pointer-typed field through an element gains exactly one deref", "[sema][pointer]")
{
    // the widened result type reaches as_value(), which is what inserts the auto-deref. a field
    // that is itself a pointer must gain one and only one - the same rule a varref base follows
    auto d = desc(
        "struct H { int& $t; }\n"
        "$v = 7;\n"
        "$h = H(&$v);\n"
        "ptr<H> $p = &$h;\n"
        "echo $p:$[0]->t;\n");

    REQUIRE(contains(d, "deref<int32>(ma<int32&>(index<H>("));
    REQUIRE_FALSE(contains(d, "deref<int32>(deref<"));
}

TEST_CASE("'->' on a peeled pointer is rejected rather than aliased", "[sema][pointer]")
{
    // `->` already reaches through every pointer level, so `$p:$->x` could only mean `$p->x`.
    // it is an error instead: `:$` marks an operation on the address, and the pointer object has
    // no members of its own (todo/B9 - the rule is the feature)
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct P { int $x; }\n"
        "$pt = P(1);\n"
        "ptr<P> $p = &$pt;\n"
        "echo $p:$->x;\n");

    REQUIRE(bundle->collector.has_critical_issues());

    bool found = false;
    for (const auto &issue : bundle->collector.issues) {
        if (issue->message().find("names the pointer itself, which has no members") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("null takes the pointer type of what it is compared against", "[sema][pointer]")
{
    // null has no type of its own. binding happens here rather than in the parser because the
    // other side's pointer-ness is only settled once the derefs above are in place
    auto d = desc("ptr<int> $p = null;\necho ($p:$ == null);\n");

    REQUIRE(contains(d, "varref<ptr<int32>>(var($p)) == null<ptr<int32>>"));
}

TEST_CASE("null stays unbound against a non-pointer, and is reported", "[sema][pointer]")
{
    // `$p == null` would auto-deref and compare the int32 at the address against null - the very
    // crash the check exists to prevent
    EchoTests::assert_code_emits_issue(
        "ptr<int> $p = null;\necho ($p == null);\n",
        "cannot compare 'int32' against null - null-check the address with ':$'");
}

TEST_CASE("':$' on a non-pointer is reported after monomorphization", "[sema][pointer]")
{
    // deferred to this pass rather than the parser because a type parameter's pointer-ness is
    // not known until it has been substituted
    EchoTests::assert_code_emits_issue(
        "$x = 5;\n$y = $x:$;\n",
        "':$' expects a pointer, got 'int32'");
}

TEST_CASE("A generic template body is left alone until it is instantiated", "[sema][pointer][generics]")
{
    // a template's parameter types are not concrete, so a deref decided there would be decided on
    // the wrong information. only the cloned instances are adjusted
    auto d = desc(
        "function box<T>(T $v) : T { return $v; }\n"
        "$var = 7;\n"
        "int& $p = &$var;\n"
        "echo box($p);\n");

    // the instance reads its argument through - T bound to int32, so the borrow decayed
    REQUIRE(contains(d, "deref<int32>(varref<int32&>(var($p)))"));

    // and no deref was written into a body whose T is still open
    REQUIRE_FALSE(contains(d, "deref<T>"));
}
