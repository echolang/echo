#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTCopy.h>
#include <AST/ASTDestruction.h>
#include <AST/ReleaseNode.h>
#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ReturnNode.h>
#include <AST/ScopeNode.h>
#include <AST/VarDeclNode.h>

#include <algorithm>

#include "helpers.h"

using namespace AST;

using EchoTests::decls_named;
using EchoTests::has_issue_containing;
using EchoTests::count_issues_containing;
using EchoTests::prim;

namespace
{
    ValueType int32_type()
    {
        return prim(ValueTypePrimitive::t_int32);
    }

    // the first closure literal in the module, or null. a closure is anonymous to the user but not to
    // the arena, so it is found by the flag rather than by a name
    ClosureExprNode *first_closure(AST::Module &m)
    {
        for (auto *node : m.nodes.of_type<ClosureExprNode>()) {
            return node;
        }

        return nullptr;
    }

    IndirectCallExprNode *first_indirect_call(AST::Module &m)
    {
        for (auto *node : m.nodes.of_type<IndirectCallExprNode>()) {
            return node;
        }

        return nullptr;
    }

    // the declaration of a closure body, of which a test source has exactly one
    FunctionDeclNode *closure_body(AST::Module &m)
    {
        for (auto *decl : m.nodes.of_type<FunctionDeclNode>()) {
            if (decl->is_closure) {
                return decl;
            }
        }

        return nullptr;
    }
}

// -- the type ----------------------------------------------------------------

TEST_CASE("a callable type written twice is one type", "[callable]")
{
    // structural, unlike a struct: a signature carries no identity of its own, so a callback could
    // never be passed anywhere if two spellings compared unequal. this is the property the whole kind
    // exists for, and the one that fails silently if operator== has no arm
    const ValueType a = ValueType::make_callable(int32_type(), { int32_type() });
    const ValueType b = ValueType::make_callable(int32_type(), { int32_type() });

    REQUIRE(a == b);
    REQUIRE(std::hash<ValueType>{}(a) == std::hash<ValueType>{}(b));
}

TEST_CASE("callable types that differ anywhere are different types", "[callable]")
{
    const ValueType base = ValueType::make_callable(int32_type(), { int32_type() });

    // the return type
    REQUIRE_FALSE(base == ValueType::make_callable(ValueType::make_void(), { int32_type() }));

    // a parameter type
    REQUIRE_FALSE(base == ValueType::make_callable(int32_type(), { prim(ValueTypePrimitive::t_float64) }));

    // the arity
    REQUIRE_FALSE(base == ValueType::make_callable(int32_type(), {}));
    REQUIRE_FALSE(base == ValueType::make_callable(int32_type(), { int32_type(), int32_type() }));
}

TEST_CASE("two different callable types get different mangled names", "[callable]")
{
    // without a mangling of its own every callable shared the `UA` unknown token, so two distinct
    // signatures produced one LLVM symbol
    const ValueType a = ValueType::make_callable(int32_type(), { int32_type() });
    const ValueType b = ValueType::make_callable(ValueType::make_void(), {});

    REQUIRE(a.get_mangled_name() != b.get_mangled_name());
    REQUIRE(a.get_mangled_name() == ValueType::make_callable(int32_type(), { int32_type() }).get_mangled_name());
}

TEST_CASE("a callable renders as it is written", "[callable]")
{
    REQUIRE(
        ValueType::make_callable(int32_type(), { int32_type(), ValueType::make_void() }).get_type_desciption()
        == "function<int32(int32, void)>");
}

TEST_CASE("the callable type parses in every position", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Holder { function<int32(int32)> $op; }\n"
        "function apply(function<int32(int32)> $f, int32 $v) : int32 { return $f($v); }\n"
        "function pick() : function<int32(int32)> {\n"
        "    return function(int32 $a) : int32 { return $a; };\n"
        "}\n"
        "function<int32(int32)> $local = pick();\n"
        "echo apply($local, 1);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // the parameter, the return type and the property all carry the same one type
    auto applies = decls_named(m, "apply");
    REQUIRE(applies.size() == 1);
    REQUIRE(applies[0]->parameter_type(0) == ValueType::make_callable(int32_type(), { int32_type() }));

    auto picks = decls_named(m, "pick");
    REQUIRE(picks.size() == 1);
    REQUIRE(picks[0]->get_return_type() == ValueType::make_callable(int32_type(), { int32_type() }));
}

TEST_CASE("a callable is not implicitly convertible to a different signature", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function apply(function<int32(int32)> $f) : int32 { return $f(1); }\n"
        "echo apply(function(float64 $a) : int32 { return 1; });\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot implicitly convert 'function<int32(float64)>'"));
}

// -- the literal -------------------------------------------------------------

TEST_CASE("a closure literal is an anonymous declaration hoisted to the file root", "[callable]")
{
    // it is not a special kind of function - only one nobody can name. so it is an ordinary
    // declaration in the file root's children, which is what codegen emits bodies from
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function<int32(int32)> $f = function(int32 $a) : int32 { return $a + 1; };\n"
        "echo $f(41);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    FunctionDeclNode *body = closure_body(m);
    REQUIRE(body != nullptr);
    REQUIRE(body->body != nullptr);

    REQUIRE(EchoTests::is_file_root_child(m, body));

    // in no overload set: no name reaches a closure, so `closure$0(...)` must not resolve
    REQUIRE(bundle->collector.functions.overloads(body->func_name(), *body->ast_namespace).empty());
}

TEST_CASE("a closure's environment parameter is implicit", "[callable]")
{
    // `args[0]` is the environment, exactly where a method's receiver sits - so it is absent from the
    // *type*, which is a promise to the caller and says nothing about what was captured
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function<int32(int32)> $f = function(int32 $a) : int32 { return $a; };\n"
        "echo $f(1);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    FunctionDeclNode *body = closure_body(m);
    REQUIRE(body != nullptr);

    REQUIRE(body->implicit_arg_count() == 1);
    REQUIRE(body->args.size() == 2);
    REQUIRE(body->args[0]->token_varname.value() == "$__env");
    REQUIRE(body->callable_type() == ValueType::make_callable(int32_type(), { int32_type() }));
}

TEST_CASE("two closure literals get different symbols", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function<int32()> $a = function() : int32 { return 1; };\n"
        "function<int32()> $b = function() : int32 { return 2; };\n"
        "echo $a() + $b();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    std::vector<FunctionDeclNode *> closures;
    for (auto *decl : m.nodes.of_type<FunctionDeclNode>()) {
        if (decl->is_closure) {
            closures.push_back(decl);
        }
    }

    REQUIRE(closures.size() == 2);
    REQUIRE(closures[0]->decorated_func_name() != closures[1]->decorated_func_name());
}

TEST_CASE("a closure cannot be written where a type parameter is visible", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer<T>(T $v) : int32 {\n"
        "    function<int32()> $f = function() : int32 { return 1; };\n"
        "    return $f();\n"
        "}\n"
        "echo outer<int32>(1);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot be written inside a generic function's body"));
}

// -- the indirect call -------------------------------------------------------

TEST_CASE("an indirect call takes its type from its callee", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function<int32(int32)> $f = function(int32 $a) : int32 { return $a; };\n"
        "echo $f(1);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    IndirectCallExprNode *call = first_indirect_call(m);
    REQUIRE(call != nullptr);
    REQUIRE(call->result_type() == int32_type());
    REQUIRE(call->arguments.size() == 1);
}

TEST_CASE("calling a non-callable is rejected", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "int32 $x = 1;\n"
        "echo $x(1);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "which cannot be called"));
}

TEST_CASE("an indirect call checks its arity", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function<int32(int32)> $f = function(int32 $a) : int32 { return $a; };\n"
        "echo $f(1, 2);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "argument(s), but 2 were given"));
}

TEST_CASE("an indirect call's argument types are checked against the signature", "[callable]")
{
    // there is no declaration to walk here, so the parameter list comes off the callee's signature. left
    // unchecked the mismatch reached LLVM and became an internal compiler error instead of a source one
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct P { int32 $x; }\n"
        "function<int32(int32)> $f = function(int32 $a) : int32 { return $a; };\n"
        "P $p = P(1);\n"
        "echo $f($p);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "Argument 1 of 'function<int32(int32)>' expects type 'int32' but got 'P'"));
}

TEST_CASE("a callable stored in a property is callable", "[callable]")
{
    // `$h->op(21)` is a call through a *value*, not a method call - so it has to be recognised before
    // parse_member_call commits, which it does as soon as a `(` follows
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Handler { function<int32(int32)> $op; }\n"
        "Handler $h = Handler(function(int32 $a) : int32 { return $a * 2; });\n"
        "echo $h->op(21);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    IndirectCallExprNode *call = first_indirect_call(m);
    REQUIRE(call != nullptr);
    REQUIRE(call->callee != nullptr);
    REQUIRE(call->callee->get_node_type() == NodeType::n_member_access);
}

// -- capture -----------------------------------------------------------------

TEST_CASE("reading an enclosing local from a closure captures it", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    int32 $n = 5;\n"
        "    function<int32()> $f = function() : int32 { return $n + 1; };\n"
        "    return $f();\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    ClosureExprNode *closure = first_closure(m);
    REQUIRE(closure != nullptr);

    // one capture, one property, and the two are in step - the store loop walks them by index
    REQUIRE(closure->captured_values.size() == 1);
    REQUIRE(closure->environment_type != nullptr);
    REQUIRE(closure->environment_type->property_count() == 1);
    REQUIRE(closure->environment_type->has_property("$n"));
    REQUIRE(closure->environment_type->is_class_kind());
}

TEST_CASE("one variable read twice is captured once", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    int32 $n = 5;\n"
        "    function<int32()> $f = function() : int32 { return $n + $n; };\n"
        "    return $f();\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    ClosureExprNode *closure = first_closure(m);
    REQUIRE(closure != nullptr);
    REQUIRE(closure->captured_values.size() == 1);
    REQUIRE(closure->environment_type->property_count() == 1);
}

TEST_CASE("a closure that captures nothing has no environment", "[callable]")
{
    // the reason a callable is a fat pointer rather than a handle: a non-capturing closure - which
    // includes every one written today in a callback position - allocates nothing at all
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function<int32(int32)> $f = function(int32 $a) : int32 { return $a + 1; };\n"
        "echo $f(41);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    ClosureExprNode *closure = first_closure(m);
    REQUIRE(closure != nullptr);
    REQUIRE(closure->environment_type == nullptr);
    REQUIRE(closure->captured_values.empty());
}

TEST_CASE("capturing an owning value is refused", "[callable]")
{
    // by value means a copy, and copying an owner is a whole taxonomy - a retain, a copy constructor,
    // or nothing that exists. the environment's teardown is uniform precisely because it holds no
    // owner, so admitting one would leak it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Buffer { usize $len; destructor() { echo 1; } }\n"
        "function outer() : usize {\n"
        "    Buffer $b = Buffer(3);\n"
        "    function<usize()> $f = function() : usize { return $b->len; };\n"
        "    return $f();\n"
        "}\n"
        "echo outer();\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "which owns a resource. Capturing an owning value is not supported yet"));
}

TEST_CASE("capturing through an enclosing closure is refused", "[callable]")
{
    // the value has to be read where it lives, and that place is not reachable from the inner
    // closure's creation site - so it is refused rather than read out of the wrong frame
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    int32 $n = 5;\n"
        "    function<int32()> $f = function() : int32 {\n"
        "        function<int32()> $g = function() : int32 { return $n; };\n"
        "        return $g();\n"
        "    };\n"
        "    return $f();\n"
        "}\n"
        "echo outer();\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "Capturing through a closure is not supported yet"));
}

// -- ownership ---------------------------------------------------------------

TEST_CASE("a callable owns its environment", "[callable]")
{
    // and says so without looking at what was captured, for the reason a class does: the signature is
    // the type, so two callables of one type may hold different environments
    REQUIRE(needs_destruction(ValueType::make_callable(ValueType::make_void(), {})));
    REQUIRE(classify_copy(ValueType::make_callable(ValueType::make_void(), {})) == CopyKind::t_retain);
}

TEST_CASE("copying a callable retains, and both copies are released", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    int32 $n = 21;\n"
        "    function<int32()> $a = function() : int32 { return $n; };\n"
        "    function<int32()> $b = $a;\n"
        "    return $a() + $b();\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // the copy is a retain, exactly as a class-typed copy is
    size_t retains = 0;
    for (auto *node : m.nodes.of_type<RetainExprNode>()) {
        if (node->result_type().is_callable()) {
            retains++;
        }
    }
    REQUIRE(retains == 1);

    // and both are given back on the way out, innermost first
    auto outers = decls_named(m, "outer");
    REQUIRE(outers.size() == 1);

    ReturnNode *ret = nullptr;
    for (auto &child : outers[0]->body->children) {
        if (child.has_type<ReturnNode>()) {
            ret = &child.get<ReturnNode>();
        }
    }

    REQUIRE(ret != nullptr);

    size_t releases = 0;
    for (auto &drop : ret->unwind) {
        if (drop.has_type<ReleaseNode>()) {
            releases++;
        }
    }
    REQUIRE(releases == 2);
}

TEST_CASE("a return's drops ride on the return, not ahead of it", "[callable]")
{
    // `return $c->x` over an owning `$c` used to free the block and then read it, because the drops
    // were statements *before* the ReturnNode and codegen evaluates the expression after them
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "class C { int32 $x; }\n"
        "function f() : int32 {\n"
        "    C $c = C(7);\n"
        "    return $c->x;\n"
        "}\n"
        "echo f();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto fs = decls_named(m, "f");
    REQUIRE(fs.size() == 1);

    ReturnNode *ret = nullptr;
    size_t statements_before = 0;
    for (auto &child : fs[0]->body->children) {
        if (child.has_type<ReturnNode>()) {
            ret = &child.get<ReturnNode>();
            break;
        }
        if (child.has_type<ReleaseNode>()) {
            statements_before++;
        }
    }

    REQUIRE(ret != nullptr);
    REQUIRE(statements_before == 0);
    REQUIRE(ret->unwind.size() == 1);
    REQUIRE(ret->unwind[0].has_type<ReleaseNode>());
}

// -- the type in aggregates and behind indirection ---------------------------

TEST_CASE("a callable is usable as a generic argument", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box<T> { T $value; }\n"
        "Box<function<int32()>> $b = Box<function<int32()>>(function() : int32 { return 42; });\n"
        "echo $b->value();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("a callable substitutes inside a generic owner's method", "[callable]")
{
    // `function<T(T)>` has to substitute structurally, or the parameter stays generic in the instance
    // and TypeLowering throws on the `T` far from where it came from
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Twice<T> {\n"
        "    T $value;\n"
        "    function run(function<T(T)> $f) : T { return $f($f($this->value)); }\n"
        "}\n"
        "Twice<int32> $t = Twice<int32>(10);\n"
        "echo $t->run(function(int32 $a) : int32 { return $a + 1; });\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("a callable type contains a type parameter structurally", "[callable]")
{
    // the counterpart in the type layer: answering false here made the monomorphizer stop chasing it
    const ValueType param = ValueType::make_callable(ValueType::make_void(), {});
    REQUIRE_FALSE(contains_type_param(param));
}

TEST_CASE("a generic inferred through a callable parameter instantiates per argument", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function apply<T>(function<T(T)> $f, T $v) : T { return $f($v); }\n"
        "echo apply(function(int32 $a) : int32 { return $a + 1; }, 41);\n"
        "echo apply(function(float64 $a) : float64 { return $a * 2.0; }, 21.0);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // two instances, one per type argument
    size_t instances = 0;
    for (auto *decl : m.nodes.of_type<FunctionDeclNode>()) {
        if (decl->func_name() == "apply" && decl->is_instantiated()) {
            instances++;
        }
    }
    REQUIRE(instances == 2);
}

TEST_CASE("a callable is reachable through a borrow and through a pointer", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function<int32()> $f = function() : int32 { return 42; };\n"
        "function apply_ref(function<int32()>& $g) : int32 { return $g(); }\n"
        "ptr<function<int32()>> $p = &$f;\n"
        "echo apply_ref($f);\n"
        "echo $p();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("a callable type declared in one file is usable from another", "[callable]")
{
    // structural equality is what makes this work at all: the two files never share a declaration
    auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string>{
        "echo apply(function(int32 $a) : int32 { return $a * 2; }, 21);\n",
        "function apply(function<int32(int32)> $f, int32 $v) : int32 { return $f($v); }\n",
    });

    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

// -- what a callable is not --------------------------------------------------

TEST_CASE("echo cannot print a callable", "[callable]")
{
    // reported by the checker, because ExprCodegen has no printf conversion for it and throws an
    // *internal compiler error* - not the user's mistake to read
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function<int32()> $f = function() : int32 { return 1; };\n"
        "echo $f;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "'echo' has no way to print a 'function<int32()>'"));
}

TEST_CASE("a callable has no null, at any of the four arrival sites", "[callable]")
{
    // the rule used to be spelled per site and three of them were missing it, so a null callable
    // reached codegen as a null aggregate: two of the sites crashed the compiler and one silently
    // stored a value that faults when called
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function<int32()> $decl = null;\n"
        "function<int32()> $ok = function() : int32 { return 1; };\n"
        "$ok = null;\n"
        "function takes(function<int32()> $f) : int32 { return $f(); }\n"
        "function gives() : function<int32()> { return null; }\n"
        "echo takes(null);\n");

    REQUIRE(bundle->collector.has_critical_issues());

    // one per site: declaration, assignment, argument, return. counted on the *reason*, which is the one
    // thing all four share - each site frames it for itself ("cannot be null", "cannot assign null to",
    // "cannot return null as"), which is the point of the predicate answering with a reason
    REQUIRE(count_issues_containing(*bundle, "a callable has no empty value") == 4);
}

TEST_CASE("a borrow still refuses null, through the same predicate", "[callable]")
{
    // the callable rule shares `null_rejection_reason` with the borrow rule, so this is the guard that
    // the shared predicate did not change what a borrow answers
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "int32& $r = null;\n"
        "function f(int32& $b) : int32 { return $b; }\n"
        "echo f(null);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(count_issues_containing(*bundle, "cannot be null") == 2);
    REQUIRE(has_issue_containing(*bundle, "declare it as a nullable pointer instead"));
}

TEST_CASE("a nullable pointer still accepts null", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle("ptr<int32> $p = null;\necho 1;\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());
}

TEST_CASE("instanceof rejects a callable operand", "[callable]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function<int32()> $f = function() : int32 { return 1; };\n"
        "echo $f instanceof int32;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "'instanceof' needs a class or an interface on the left"));
}

// -- recovery ----------------------------------------------------------------

TEST_CASE("a malformed callable type reports rather than crashing", "[callable]")
{
    // the shape walk uses only the bounds-safe cursor accessors, so a truncated type answers false
    // instead of running off the end
    for (const char *source : { "function<> $f;\n", "function<int32> $f;\n", "function<int32( $f;\n" }) {
        auto bundle = EchoTests::tests_make_parsed_bundle(source);
        REQUIRE(bundle->collector.has_critical_issues());
    }
}

TEST_CASE("a malformed closure literal reports rather than crashing", "[callable]")
{
    for (const char *source : {
        "function<int32()> $f = function() : int32;\n",
        "function<int32()> $f = function( : int32 { return 1; };\n",
        "function<int32()> $f = function() : int32 { return 1;\n",
    }) {
        auto bundle = EchoTests::tests_make_parsed_bundle(source);
        REQUIRE(bundle->collector.has_critical_issues());
    }
}

TEST_CASE("a refused closure inside a return recovers", "[callable]")
{
    // the refusal's recovery has to skip the literal brace-aware. skipping to the next `;` landed inside
    // the body and ran the cursor off the end of the file, which asserted in Cursor::current
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function mk<T>(T $seed) : int32 { return function() : int32 { return 1; }; }\n"
        "echo 1;\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(count_issues_containing(*bundle, "cannot be written inside a generic function's body") == 1);
    REQUIRE(bundle->collector.issues.size() == 1);
}

TEST_CASE("a cloned body gets its own indirect call and closure nodes", "[callable]")
{
    // the monomorphizer clones a generic body per instance, so both new node kinds have to clone -
    // sharing a node between two instances would let one instance's coercion overwrite the other's
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function apply<T>(function<T(T)> $f, T $v) : T { return $f($v); }\n"
        "echo apply(function(int32 $a) : int32 { return $a + 1; }, 41);\n"
        "echo apply(function(float64 $a) : float64 { return $a * 2.0; }, 21.0);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // one per instance body, and they are distinct nodes with distinct signatures
    std::vector<IndirectCallExprNode *> calls;
    for (auto *node : m.nodes.of_type<IndirectCallExprNode>()) {
        calls.push_back(node);
    }

    REQUIRE(calls.size() >= 2);
    REQUIRE(calls[0] != calls[1]);

    // each cloned callee carries the substituted signature, which is what says substitute_type
    // recursed into it rather than handing it back unchanged
    std::vector<ValueType> return_types;
    for (auto *call : calls) {
        return_types.push_back(call->result_type());
    }

    const bool has_int = std::find(return_types.begin(), return_types.end(), int32_type()) != return_types.end();
    const bool has_float = std::find(
        return_types.begin(), return_types.end(), prim(ValueTypePrimitive::t_float64)) != return_types.end();

    REQUIRE(has_int);
    REQUIRE(has_float);
}

TEST_CASE("running out of input is reported, not aborted", "[callable]")
{
    // Cursor::current asserts past the end, so every parser reporting a missing token at end of input
    // used to kill the compiler. a truncated closure body was one shape of it; a truncated function body
    // and an unterminated `if` were the same bug and are covered here so the fix cannot regress to
    // "closures only"
    for (const char *source : {
        "function<int32()> $f = function() : int32 { return 1;\n",
        "function f() : int32 { return 1;\n",
        "function f() : int32 { if (true) { return 1;\n",
        // deliberately not `struct S { int32 $x;` - a truncated *struct* body reports nothing at all
        // today, which is a separate pre-existing gap in the member walk rather than this one
    }) {
        auto bundle = EchoTests::tests_make_parsed_bundle(source);
        REQUIRE(bundle->collector.has_critical_issues());
    }
}
