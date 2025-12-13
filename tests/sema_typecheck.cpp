#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTIssue.h>
#include <string>

#include "helpers.h"

// the semantic/type-check pass runs inside tests_make_parsed_bundle (after monomorphization),
// so these assert the diagnostics it records on the collector. errors that previously surfaced
// as context-free codegen throws (or silent voids) now become located, gated issues here

using namespace AST;

namespace
{
    // true if any recorded issue's message contains the given substring
    bool has_issue_containing(Bundle &bundle, const std::string &needle) {
        for (const auto &issue : bundle.collector.issues) {
            if (issue->message().find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("unknown struct member is a located diagnostic, not a silent void", "[sema]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct point { int $x; int $y; }\n"
        "point $p = point(1, 2);\n"
        "echo $p->z;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "has no member named 'z'"));
    // the diagnostic names the struct it was resolved against
    REQUIRE(has_issue_containing(*bundle, "'point'"));
}

TEST_CASE("unknown member behind an element base is reported too", "[sema]")
{
    // the base here is an IndexExprNode, which neither the node's own result_type() nor the type
    // checker's copy of the same switch knew about - so this went entirely unreported (todo/B16)
    // one implementation answers both now
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct point { int $x; int $y; }\n"
        "point $p = point(1, 2);\n"
        "ptr<point> $ptr = &$p;\n"
        "echo $ptr:$[0]->z;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "has no member named 'z'"));
    REQUIRE(has_issue_containing(*bundle, "'point'"));
}

TEST_CASE("a const field written through an element base is rejected", "[sema]")
{
    // check_const_target keys on the target's storage type, which was void for this shape - so
    // the guard silently did not fire
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct box { const int $v; }\n"
        "box $b = box(1);\n"
        "ptr<box> $p = &$b;\n"
        "$p:$[0]->v = 2;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "const"));
}

TEST_CASE("valid struct member access produces no critical issues", "[sema]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct point { int $x; int $y; }\n"
        "point $p = point(1, 2);\n"
        "echo $p->x;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("wrong-type argument is a located diagnostic", "[sema]")
{
    // passing a struct where an int parameter is expected: the implicit cast the parser inserts
    // is not a legal conversion, and is caught here instead of crashing codegen with
    // "Unsupported type cast"
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct point { int $x; int $y; }\n"
        "function takes_int(int $n): int { return $n; }\n"
        "point $p = point(1, 2);\n"
        "takes_int($p);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot implicitly convert 'point' to 'int32'"));
}

TEST_CASE("correct-type arguments produce no critical issues", "[sema]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function add(int $a, int $b): int { return $a + $b; }\n"
        "echo add(2, 3);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("numeric argument conversions are allowed", "[sema]")
{
    // a float argument to an int parameter is a legal implicit numeric conversion and must not
    // be flagged as a type error
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function takes_int(int $n): int { return $n; }\n"
        "echo takes_int(3);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("use of an unresolvable generic is a critical diagnostic", "[sema]")
{
    // the monomorphizer cannot resolve a call with the wrong number of explicit type arguments;
    // the pipeline reports it as a critical issue rather than reaching codegen. (leftover type
    // parameters in already-concrete code are additionally caught by the type-check pass.)
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function id<T>(T $x): T { return $x; }\n"
        "echo id<int, int>(5);\n");

    REQUIRE(bundle->collector.has_critical_issues());
}

TEST_CASE("a valid generic instantiation type-checks cleanly", "[sema]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function twice<T>(T $x): T { return $x + $x; }\n"
        "echo twice(5);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("a binary operator on struct operands is a located diagnostic", "[sema]")
{
    // codegen supports no operator on struct operands; it would otherwise surface as a context-free
    // "unsupported binary operator" deep in codegen. the type-check pass catches it up-front, located
    // at the operator
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct point { int $x; int $y; }\n"
        "point $a = point(1, 2);\n"
        "point $b = point(3, 4);\n"
        "echo $a + $b;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "operator '+'"));
    // the diagnostic names the operand type it was rejected on
    REQUIRE(has_issue_containing(*bundle, "'point'"));
}

TEST_CASE("numeric binary operators are not flagged", "[sema]")
{
    // int + int and a float/int mix are legal for codegen; the struct-operand check must not fire.
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "echo 1 + 2;\n"
        "echo 1.5 * 2;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

// ---------------------------------------------------------------------------
// pointer diagnostics
//
// the same conditions are covered end-to-end in tests_eco/errors/, which pins the exact rendered
// block. these are the cheap, precisely located counterparts - and they can assert the thing a
// golden cannot: that a *legal* program produces no diagnostic at all. an over-eager pointer
// check is as much a bug as a missing one, and only a negative control catches it

TEST_CASE("assigning an address into a pointee is rejected", "[sema][pointer]")
{
    // `$p = &$b` is not a rebind: a plain assignment writes through, so this stores an address
    // into the int32 that $p points at (book/concept/pointers_and_refs_v2.md, "Binding, writing, and re-seating")
    EchoTests::assert_code_emits_issue(
        "$a = 1;\n$b = 2;\nptr<int> $p = &$a;\n$p = &$b;\n",
        "Invalid type conversion: cannot assign 'int32&' to 'int32' - to change where a pointer points, assign to ':$'");
}

TEST_CASE("a nullable pointer does not narrow to a borrow on its own", "[sema][pointer]")
{
    EchoTests::assert_code_emits_issue(
        "$a = 5;\nptr<int> $p = &$a;\nint& $r = $p:$;\n",
        "Invalid type conversion: cannot implicitly convert 'ptr<int32>' to 'int32&' - write the cast explicitly to assert it is not null");
}

TEST_CASE("a borrow cannot be seeded with null", "[sema][pointer]")
{
    EchoTests::assert_code_emits_issue(
        "int& $r = null;\n",
        "'int32&' cannot be null - declare it as a nullable pointer instead");
}

TEST_CASE("a ptr<T> parameter does not auto-borrow", "[sema][pointer]")
{
    // only `T&` borrows implicitly. a nullable pointer parameter can be null, so taking an
    // address is a decision the caller should be able to see in the source (doc L135)
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function maybe_inc(ptr<int> $x) : void { $x = $x + 1; }\n"
        "$a = 5;\n"
        "maybe_inc($a);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot implicitly convert 'int32' to 'ptr<int32>'"));
}

TEST_CASE("returning the address of a local is rejected", "[sema][pointer]")
{
    // the storage is gone before the caller sees it (doc, "Lifetimes, and how to get hurt")
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function bad() : int& {\n"
        "    $local = 5;\n"
        "    return &$local;\n"
        "}\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot return the address of local '$local'"));
}

TEST_CASE("returning a value where a borrow is declared is rejected", "[sema][pointer]")
{
    // this used to reach codegen and fail llvm's verifier with "Function return type does not
    // match operand type of return inst", which named neither the function nor the line
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function bad(int $x) : int& {\n"
        "    return $x;\n"
        "}\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot return 'int32' from a function declared 'int32&'"));
}

// the four below are regression guards for crashes. each aborted or segfaulted the compiler
// before, with no location and nothing the user could act on - the assertions here are as much
// "this terminates and reports" as they are about the wording

TEST_CASE("taking the address of something with no storage is a diagnostic", "[sema][pointer]")
{
    // both used to reach `assert(false && "unimplemented")` at the end of the operand
    // production, aborting the compiler. note `&($a + $b)` takes a different path - the lexer
    // never emits t_ref before `(` - which is why that case alone gave false confidence
    EchoTests::assert_code_emits_issue(
        "$a = 1;\nint& $r = &5;\n",
        "Cannot take the address of an expression that has no storage");

    EchoTests::assert_code_emits_issue(
        "function get() : int { return 5; }\nint& $r = &get();\n",
        "Cannot take the address of an expression that has no storage");
}

TEST_CASE("an address only compares against another address", "[sema][pointer]")
{
    // llvm asserts on an icmp whose operands differ in type, so this aborted inside codegen
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "$a = 1;\nptr<int> $p = &$a;\necho ($p:$ == 0);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot compare 'ptr<int32>' against 'int32'"));
}

TEST_CASE("pointer arithmetic still mixes an address with an integer", "[sema][pointer]")
{
    // the negative control for the case above: offsetting is not comparing, so the check must
    // not widen to every binary operator
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct P { int $a; int $b; }\n"
        "$s = P(1, 2);\n"
        "ptr<int> $p = &$s->a;\n"
        "ptr<int> $q = $p:$ + 1;\n"
        "echo ($q:$ - $p:$);\n");

    for (const auto &issue : bundle->collector.issues) {
        INFO("unexpected issue: " << issue->message());
    }
    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("null cannot be passed to a borrow parameter", "[sema][pointer]")
{
    // the declaration site already refused this; the call site did not, and the callee
    // segfaulted on the first read through the parameter. the null arrives wrapped in an
    // implicit cast, so the check has to look through one to find the n_null tag
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f(int &$x) : void { $x = 1; }\n"
        "f(null);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "argument 1 of 'f' is 'int32&', which cannot be null"));
}

TEST_CASE("null is still accepted by a nullable pointer parameter", "[sema][pointer]")
{
    // the negative control: only the non-null guarantee of a borrow is at stake
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f(ptr<int> $x) : void { }\n"
        "f(null);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("legal pointer programs are left alone", "[sema][pointer]")
{
    // the negative control. every conversion here is one the doc permits, and each was at some
    // point rejected by a check that was drawn too wide
    auto bundle = EchoTests::tests_make_parsed_bundle(
        // a borrow widens to a nullable pointer
        "$a = 1;\n"
        "$b = 2;\n"
        "int& $r = &$a;\n"
        "ptr<int> $p = $r:$;\n"
        // re-seating through :$, and writing through without it
        "$p:$ = &$b;\n"
        "$p = 20;\n"
        // null into a nullable pointer, and the address-side null check
        "ptr<int> $empty = null;\n"
        "echo ($empty:$ == null);\n"
        // the explicit narrowing back - still permitted, and it is the one line here that costs the
        // word: turning a raw address into a trusted borrow is the promotion, not the conversion
        "unsafe { int& $back = int&($p:$); }\n"
        // a read-only borrow parameter accepting a mutable argument
        "function show(const int& $v) : void { echo $v; }\n"
        "show($a);\n"
        // a borrow returned from a borrow parameter
        "function pick(int &$x) : int& { return $x; }\n"
        "int& $picked = pick($a);\n");

    for (const auto &issue : bundle->collector.issues) {
        INFO("unexpected issue: " << issue->message());
    }
    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("writing through a pointer to const is rejected", "[sema][pointer][const]")
{
    // `const int&` is a *mutable borrow of a const pointee*, so the const sits one level below the
    // variable's own type. the old check looked at the top level only and so never fired here
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "const int $var = 10;\n"
        "const int& $ref = &$var;\n"
        "$ref = 20;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot write through 'const int32&' - its pointee is const"));
}

TEST_CASE("writing through a ptr<const T> is rejected", "[sema][pointer][const]")
{
    // the nullable spelling of the same rule - const is independent of nullability
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "const int $a = 1;\n"
        "ptr<const int> $p = &$a;\n"
        "$p = 9;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot write through 'ptr<const int32>' - its pointee is const"));
}

TEST_CASE("re-seating a const pointer is rejected", "[sema][pointer][const]")
{
    // the mirror case: here the const is on the pointer level, so it is the slot that is frozen
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "$a = 1;\n"
        "$b = 2;\n"
        "const ptr<int> $p = &$a;\n"
        "$p:$ = &$b;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot re-seat 'const ptr<int32>'"));
}

TEST_CASE("assigning to a const variable is rejected by the type checker", "[sema][const]")
{
    // this used to be a parser diagnostic reported as a "redeclaration" on the variable's name
    // it moved to the assignment target, which is the only place that can tell the four const
    // cases apart, so it now reports at the `=` like every other assignment error
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "const int $v = 1;\n"
        "$v = 2;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot assign to '$v' - it is declared const"));

    // an inferred const carries the same promise
    auto inferred = EchoTests::tests_make_parsed_bundle(
        "const $c = 7;\n"
        "$c = 8;\n");

    REQUIRE(inferred->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*inferred, "cannot assign to '$c' - it is declared const"));
}

TEST_CASE("assigning to a const struct property is rejected", "[sema][const][struct]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point { const int $x; int $y; }\n"
        "$p = Point(3, 4);\n"
        "$p->x = 9;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot assign to 'x' - it is declared const"));
}

TEST_CASE("legal const programs are left alone", "[sema][pointer][const]")
{
    // the negative control for the const rules. each of these is permitted by the doc's "Const"
    // section and each is one half of a pair whose other half is rejected above - which is the
    // point: the level the const sits on decides, so a check drawn at the wrong level breaks these
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "$a = 1;\n"
        "$b = 2;\n"
        // a const *pointer* may be written through - what it forbids is re-seating
        "const ptr<int> $p = &$a;\n"
        "$p = 5;\n"
        // and a const *pointee* may be re-seated - what it forbids is the write-through
        "const int $c = 1;\n"
        "const int $d = 2;\n"
        "const int& $view = &$c;\n"
        "$view:$ = &$d;\n"
        // reading through either is always fine
        "echo $view;\n"
        "echo $p;\n"
        // a const property is written exactly once, by the constructor the parser synthesizes
        "struct Point { const int $x; int $y; }\n"
        "$pt = Point(3, 4);\n"
        "echo $pt->x;\n"
        // a mutable borrow is still writable
        "int& $r = &$b;\n"
        "$r = 20;\n");

    for (const auto &issue : bundle->collector.issues) {
        INFO("unexpected issue: " << issue->message());
    }
    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("a per-branch operator gap over primitives is reported here", "[sema]")
{
    // this used to pin the opposite: the sema check was scoped to *struct* operands, and a primitive
    // operator/operand gap was deliberately left to the codegen throw. that throw carries no location,
    // no source excerpt and no exit status a user can act on, and AST::binary_has_builtin_meaning was
    // answering *true* for every one of these - which is a promise ExprCodegen::gen_binary_expr cannot
    // keep. the predicate enumerates what that function lowers now, so each gap arrives here instead
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "echo true % false;\n");

    REQUIRE(has_issue_containing(*bundle, "operator '%' is not supported on operands of type 'bool'"));
}

TEST_CASE("the gaps that are gone report nothing", "[sema]")
{
    // the negative control for the one above, and the two halves of the ticket that opened it: a bool
    // equality and a weak handle asked whether it is there. both used to be accepted by the predicate
    // and then abort codegen; both lower now, so neither may draw a diagnostic here
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "class C { int32 $v; }\n"
        "bool $a = true;\n"
        "bool $b = false;\n"
        "echo $a == $b;\n"
        "echo $a != $b;\n"
        "C $o = C(1);\n"
        "weak<C> $w = &$o;\n"
        "echo $w == null;\n"
        "echo $w != null;\n");

    for (const auto &issue : bundle->collector.issues) {
        INFO("unexpected issue: " << issue->message());
    }
    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("live_allocations is refused when nothing is counting", "[sema][memory]")
{
    // the only builtin whose *availability* is a question, and the only reason AST::TypeChecker reads the
    // compiler options at all. without --track-allocations there is no counter, and a load of one would
    // answer 0 - which is the single wrong answer a caller cannot distinguish from the right one, since
    // the whole reason to write this call is to assert that a program came back to zero
    //
    // **the e2e corpus cannot test this.** tests/eco_test_file.cpp passes --track-allocations to every
    // case deliberately, so there is no `.test` shaped  like its absence - which is what this is for
    const std::string program =
        "#[builtin: \"live_allocations\"]\n"
        "function live_allocations() : usize;\n"
        "echo live_allocations();\n";

    Compiler::CompilerOptions untracked;
    untracked.track_allocations = false;

    auto refused = EchoTests::tests_make_parsed_bundle(program, untracked);

    REQUIRE(has_issue_containing(*refused,
        "'live_allocations' has nothing to read without allocation tracking"));
    REQUIRE(refused->collector.has_critical_issues());

    // and with the flag it is an ordinary call. the negative control matters here more than usual: a
    // refusal keyed on the wrong thing would reject every program rather than only the untracked ones
    Compiler::CompilerOptions tracked;
    tracked.track_allocations = true;

    auto accepted = EchoTests::tests_make_parsed_bundle(program, tracked);

    for (const auto &issue : accepted->collector.issues) {
        INFO("unexpected issue: " << issue->message());
    }
    REQUIRE_FALSE(accepted->collector.has_critical_issues());
}
