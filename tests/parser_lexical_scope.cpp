#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTNamespace.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ExprNode.h>
#include <AST/ScopeNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::calls_to;
using EchoTests::decls_named;
using EchoTests::has_issue_containing;
using EchoTests::count_issues_containing;
using EchoTests::is_file_root_child;

TEST_CASE("two bodies may each declare a helper of the same signature", "[lexical]")
{
    // the registry keyed (namespace, name) and parse_funcdecl stamped the *file's* namespace on a
    // declaration written anywhere, so these two collided as one duplicate symbol. each body's block
    // is its own declaration scope now
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    function helper(int32 $a) : int32 { return $a + 1; }\n"
        "    return helper(1);\n"
        "}\n"
        "function other() : int32 {\n"
        "    function helper(int32 $a) : int32 { return $a + 2; }\n"
        "    return helper(1);\n"
        "}\n"
        "echo outer();\n"
        "echo other();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto helpers = decls_named(m, "helper");
    REQUIRE(helpers.size() == 2);
    REQUIRE(helpers[0] != helpers[1]);

    // and each body calls its own, which is the half a shared node would have hidden
    auto calls = calls_to(m, "helper");
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0]->decl == helpers[0]);
    REQUIRE(calls[1]->decl == helpers[1]);
}

TEST_CASE("two body-local helpers get distinct symbols", "[lexical]")
{
    // the two declarations being distinct nodes is not enough: they also have to mangle apart, or
    // TypeLowering emits two bodies into one llvm::Function. the lexical namespace's *mangling* name
    // carries a per-block discriminator its display name does not
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    function helper(int32 $a) : int32 { return 1; }\n"
        "    return helper(1);\n"
        "}\n"
        "function other() : int32 {\n"
        "    function helper(int32 $a) : int32 { return 2; }\n"
        "    return helper(1);\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto helpers = decls_named(m, "helper");
    REQUIRE(helpers.size() == 2);
    REQUIRE(helpers[0]->decorated_func_name() != helpers[1]->decorated_func_name());
}

TEST_CASE("a body-local function is not visible at file scope", "[lexical]")
{
    // it used to resolve here and then jump into a declared-but-never-defined symbol, because
    // build_function_maps declares from the arena while bodies are emitted from the file root
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    function helper(int32 $a) : int32 { return $a + 1; }\n"
        "    return helper(1);\n"
        "}\n"
        "echo helper(5);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "The function 'helper' could not be found"));
}

TEST_CASE("a body-local function shadows a file-scope one of the same signature", "[lexical]")
{
    // the same rule overloads() already implements for namespaces: the innermost set with any
    // candidate for the name answers entirely, rather than extending the outer one
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function helper(int32 $a) : int32 { return 100; }\n"
        "function outer() : int32 {\n"
        "    function helper(int32 $a) : int32 { return 7; }\n"
        "    return helper(1);\n"
        "}\n"
        "echo helper(1);\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto helpers = decls_named(m, "helper");
    REQUIRE(helpers.size() == 2);

    auto calls = calls_to(m, "helper");
    REQUIRE(calls.size() == 2);

    // the inner call takes the inner declaration, the file-scope one the outer
    REQUIRE(calls[0]->decl == helpers[1]);
    REQUIRE(calls[1]->decl == helpers[0]);
}

TEST_CASE("two declarations of one signature in the same block are still a duplicate", "[lexical]")
{
    // scoping must not turn the duplicate diagnostic off - find_by_signature searches exactly one
    // namespace, which is now exactly one block
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    function helper(int32 $a) : int32 { return 1; }\n"
        "    function helper(int32 $b) : int32 { return 2; }\n"
        "    return helper(1);\n"
        "}\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "is already declared with these parameter types"));

    // reported once, not once per parse pass
    REQUIRE(count_issues_containing(*bundle, "is already declared with these parameter types") == 1);
}

TEST_CASE("a duplicate in a block names the function it was written in", "[lexical]")
{
    // the message renders the *display* path, so it reads `outer::helper(int32)` and not the mangling
    // name's discriminator - a namespace the user never wrote should not leak its number into a
    // diagnostic, but naming the enclosing function is strictly more useful than the bare signature
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    function helper(int32 $a) : int32 { return 1; }\n"
        "    function helper(int32 $b) : int32 { return 2; }\n"
        "    return helper(1);\n"
        "}\n");

    REQUIRE(has_issue_containing(*bundle, "'outer::helper(int32)' is already declared"));
}

TEST_CASE("sibling blocks may each declare the same function", "[lexical]")
{
    // names are block scoped, exactly as variables are - the disagreement between the two models is
    // what B18 was about, so a `{ }` and not a function body is the unit
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    { function h() : int32 { return 1; } echo h(); }\n"
        "    { function h() : int32 { return 2; } echo h(); }\n"
        "    return 0;\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "h");
    REQUIRE(decls.size() == 2);

    auto calls = calls_to(m, "h");
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0]->decl == decls[0]);
    REQUIRE(calls[1]->decl == decls[1]);
}

TEST_CASE("a block-local function is not visible outside its block", "[lexical]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    { function h() : int32 { return 1; } }\n"
        "    return h();\n"
        "}\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "The function 'h' could not be found"));
}

TEST_CASE("a call above the declaration in the same block resolves", "[lexical]")
{
    // this is why the declaration pass *descends* into a body rather than skipping it. a free call
    // that cannot be resolved is reported and its node discarded right there in the parser, so a name
    // registered later in the body pass's single linear walk would already be gone
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    int32 $r = helper(1);\n"
        "    function helper(int32 $a) : int32 { return $a + 1; }\n"
        "    return $r;\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto calls = calls_to(m, "helper");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->decl != nullptr);
}

TEST_CASE("two block-local functions may call each other", "[lexical]")
{
    // mutual recursion needs both names registered before either body is read, which is the
    // declaration pass's whole job - it just had to start doing it inside bodies too
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    function is_even(int32 $n) : int32 { if ($n == 0) { return 1; } return is_odd($n - 1); }\n"
        "    function is_odd(int32 $n) : int32 { if ($n == 0) { return 0; } return is_even($n - 1); }\n"
        "    return is_even(4);\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    REQUIRE(calls_to(m, "is_even").size() == 2);
    REQUIRE(calls_to(m, "is_odd").size() == 1);

    for (auto *call : calls_to(m, "is_even")) {
        REQUIRE(call->decl != nullptr);
    }
}

TEST_CASE("a block-local declaration is hoisted to the file root", "[lexical]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    function helper(int32 $a) : int32 { return $a + 1; }\n"
        "    return helper(1);\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto helpers = decls_named(m, "helper");
    REQUIRE(helpers.size() == 1);

    REQUIRE(is_file_root_child(m, helpers[0]));

    // and it is *only* there - left in the body it would be emitted by nobody, since gen_scope skips
    // a FunctionDeclNode child
    auto outers = decls_named(m, "outer");
    REQUIRE(outers.size() == 1);
    REQUIRE(outers[0]->body != nullptr);

    for (auto &child : outers[0]->body->children) {
        REQUIRE(child.node() != helpers[0]);
    }
}

TEST_CASE("reading an enclosing function's local is reported, not lowered", "[lexical]")
{
    // the scope chain is deliberately left intact - it is the environment a closure will capture from
    // - so the read resolves. lowering it would load from an alloca belonging to a different
    // llvm::Function, because CodegenContext::var_map is keyed on the declaration and never cleared
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    int32 $n = 5;\n"
        "    function bad() : int32 { return $n; }\n"
        "    return bad();\n"
        "}\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "'$n' is declared in an enclosing function"));
}

TEST_CASE("a declaration over an enclosing name is a declaration, not an assignment", "[lexical]")
{
    // parse_varexpr decides "declaration or assignment" by looking the name up, and a hit from
    // outside the frame used to make this an assignment into storage the nested body cannot address
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    int32 $x = 1;\n"
        "    function inner() : int32 { int32 $x = 2; return $x; }\n"
        "    return inner() + $x;\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto inners = decls_named(m, "inner");
    REQUIRE(inners.size() == 1);
    REQUIRE(inners[0]->body != nullptr);

    // the inner body declares its own `$x` rather than assigning to the outer one
    bool declares_own_x = false;
    for (auto &child : inners[0]->body->children) {
        if (!child.has_type<AST::VarDeclNode>()) {
            continue;
        }

        if (child.get_ptr<AST::VarDeclNode>()->token_varname.value() == "$x") {
            declares_own_x = true;
        }
    }

    REQUIRE(declares_own_x);
}

TEST_CASE("a function declared where a type parameter is visible is rejected", "[lexical]")
{
    // `T` would resolve through the type-param scope stack and make this declaration depend on a
    // substitution nothing will ever hand it. this is lifted once a closure's environment is
    // monomorphized along with the enclosing instance
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer<T>(T $v) : int32 {\n"
        "    function helper(int32 $a) : int32 { return $a; }\n"
        "    return helper(1);\n"
        "}\n"
        "echo outer<int32>(1);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot be declared inside a generic function's body"));
}

TEST_CASE("a function declared in a method body resolves", "[lexical]")
{
    // the declaration pass has to open the same frames the body pass does around a member body.
    // without the null SelfScope this registered as another *method* of the owner, the body pass then
    // found the declaration site already claimed, and the name resolved nowhere at all
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Counter {\n"
        "    int32 $count;\n"
        "    function bump() : int32 {\n"
        "        function twice(int32 $a) : int32 { return $a * 2; }\n"
        "        return twice($this->count);\n"
        "    }\n"
        "}\n"
        "Counter $c = Counter(21);\n"
        "echo $c->bump();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto twices = decls_named(m, "twice");
    REQUIRE(twices.size() == 1);

    // a free function of the block, not a member of Counter
    REQUIRE(twices[0]->owner_type == nullptr);
    REQUIRE(is_file_root_child(m, twices[0]));

    auto calls = calls_to(m, "twice");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->decl == twices[0]);
}

TEST_CASE("a function declared in a constructor body resolves", "[lexical]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Box {\n"
        "    int32 $v;\n"
        "    constructor(int32 $v) {\n"
        "        function triple(int32 $a) : int32 { return $a * 3; }\n"
        "        $this->v = triple($v);\n"
        "    }\n"
        "}\n"
        "Box $b = Box(5);\n"
        "echo $b->v;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto triples = decls_named(m, "triple");
    REQUIRE(triples.size() == 1);
    REQUIRE(triples[0]->owner_type == nullptr);

    auto calls = calls_to(m, "triple");
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->decl == triples[0]);
}

TEST_CASE("a namespace declaration inside a body is rejected", "[lexical]")
{
    // `namespace a;` names what the rest of the *file* declares into. the three passes walk blocks
    // differently, so one would follow it and the others would not - and a struct written after the
    // block would end up with two declaration nodes in two namespaces
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : void {\n"
        "    namespace inner;\n"
        "}\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot appear inside a body"));
}

TEST_CASE("a lexical namespace is unreachable by name", "[lexical]")
{
    // it lives in a map keyed on its opening brace rather than in `_children`, so no namespace path a
    // user can write reaches into it and `namespace outer;` cannot merge with it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    function helper(int32 $a) : int32 { return $a + 1; }\n"
        "    return helper(1);\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto helpers = decls_named(m, "helper");
    REQUIRE(helpers.size() == 1);

    const AST::Namespace *lexical = helpers[0]->ast_namespace;
    REQUIRE(lexical != nullptr);
    REQUIRE(lexical->is_lexical());

    // not addressable, and the nearest namespace the user could have written is the real one
    REQUIRE_FALSE(bundle->collector.namespaces.exists(lexical->name()));
    REQUIRE_FALSE(lexical->declaring_namespace()->is_lexical());

    // the display path names the enclosing function, the mangling path is unique
    REQUIRE(helpers[0]->namespaced_func_name() == "outer::helper");
    REQUIRE(lexical->name() != lexical->display_name());
}

TEST_CASE("a body-local struct's constructor is emitted", "[lexical]")
{
    // the hoist covers every declaration a body can hold, not only a `function`: a body-local struct's
    // field-wise constructor used to land in the body scope, where gen_scope skips it and nothing ever
    // emits its body
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    struct P { int32 $x; }\n"
        "    P $p = P(41);\n"
        "    return $p->x + 1;\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto ctors = decls_named(m, "P");
    REQUIRE(ctors.size() == 1);
    REQUIRE(is_file_root_child(m, ctors[0]));
}

TEST_CASE("two bodies may each declare a struct of the same name", "[lexical]")
{
    // the type half of the case above it: a type declared in a block belongs to the block, so these are
    // two types with one name rather than a redeclaration - and they must be two *layouts*, because
    // struct equality is ComplexType* identity and a shared node would type both bodies alike
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    struct P { int32 $x; }\n"
        "    P $p = P(41);\n"
        "    return $p->x;\n"
        "}\n"
        "function other() : int32 {\n"
        "    struct P { int32 $a; int32 $b; }\n"
        "    P $p = P(2, 3);\n"
        "    return $p->a * $p->b;\n"
        "}\n"
        "echo outer();\n"
        "echo other();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto types = EchoTests::types_named(m, "P");
    REQUIRE(types.size() == 2);
    REQUIRE(types[0]->properties().size() == 1);
    REQUIRE(types[1]->properties().size() == 2);

    // distinct symbols, or the two field-wise constructors land in one llvm::Function
    REQUIRE(types[0]->complex_type().mangled_token() != types[1]->complex_type().mangled_token());

    // and each body constructs its own, which is the half a shared node would have hidden
    auto ctors = decls_named(m, "P");
    REQUIRE(ctors.size() == 2);

    auto calls = calls_to(m, "P");
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0]->decl == ctors[0]);
    REQUIRE(calls[1]->decl == ctors[1]);
}

TEST_CASE("two sibling blocks of one function may each declare a struct of the same name", "[lexical]")
{
    // the case a *display*-named member surface would have passed silently: both blocks render as
    // `outer`, so anything keyed on the display path collapses them into one
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    int32 $total = 0;\n"
        "    { struct P { int32 $x; } P $p = P(10); $total = $total + $p->x; }\n"
        "    { struct P { int32 $y; int32 $z; } P $p = P(1, 2); $total = $total + $p->y + $p->z; }\n"
        "    return $total;\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto types = EchoTests::types_named(m, "P");
    REQUIRE(types.size() == 2);
    REQUIRE(types[0]->properties().size() == 1);
    REQUIRE(types[1]->properties().size() == 2);
    REQUIRE(types[0]->complex_type().mangled_token() != types[1]->complex_type().mangled_token());
}

TEST_CASE("a body-local struct is not visible at file scope", "[lexical]")
{
    // an unresolved *unqualified* type name is silent by design, so what reports this is the call:
    // `P(1)` finds no overload set anywhere on the walk out of the file's namespace
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    struct P { int32 $x; }\n"
        "    P $p = P(41);\n"
        "    return $p->x;\n"
        "}\n"
        "P $q = P(1);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "The function 'P' could not be found"));
}

TEST_CASE("a body-local struct's name lives in the block's lexical namespace", "[lexical]")
{
    // the mechanism the three cases above rest on, asserted directly: the symbol is in the block's
    // namespace and in no namespace a `namespace <x>;` could ever name
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    struct P { int32 $x; }\n"
        "    function helper() : int32 { return 1; }\n"
        "    P $p = P(41);\n"
        "    return $p->x + helper();\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // the block's namespace, read off the function declared beside the struct
    auto helpers = decls_named(m, "helper");
    REQUIRE(helpers.size() == 1);
    const AST::Namespace *lexical = helpers[0]->ast_namespace;
    REQUIRE(lexical != nullptr);
    REQUIRE(lexical->is_lexical());

    REQUIRE(bundle->collector.namespaces.find_symbol("P", *lexical) != nullptr);
    REQUIRE(bundle->collector.namespaces.find_symbol("P", bundle->collector.namespaces.root()) == nullptr);
}

TEST_CASE("a body-local struct's member surface is not a writable namespace", "[lexical]")
{
    // AST::member_surface_namespace hangs a type's constants and nested types off its namespace *object*
    // rather than off a path from the root. a path would land under a display name a block shares with
    // every sibling block, and - far worse - create that namespace as one a `namespace outer;` could
    // then merge into, which is exactly what a lexical namespace exists not to be
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    struct P {\n"
        "        const int32 MAX = 9;\n"
        "        int32 $x;\n"
        "    }\n"
        "    P $p = P(4);\n"
        "    return $p->x;\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &namespaces = bundle->collector.namespaces;
    REQUIRE_FALSE(namespaces.exists("P"));
    REQUIRE_FALSE(namespaces.exists("outer"));
    REQUIRE_FALSE(namespaces.exists("outer::P"));
}

TEST_CASE("an inner block's struct is not visible in the enclosing block", "[lexical]")
{
    // the walk is outward only, so a name declared deeper is not reachable from where the block closed -
    // the same asymmetry a nested `function` already has
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    { struct P { int32 $x; } P $inner = P(1); echo $inner->x; }\n"
        "    P $p = P(41);\n"
        "    return $p->x;\n"
        "}\n"
        "echo outer();\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "The function 'P' could not be found"));
}

TEST_CASE("a namespace declaration inside a body moves nothing", "[lexical]")
{
    // the statement is refused, and the type-name pass has to decline to *follow* it as well: that pass
    // opens no lexical scope, so the refusal below - which reads current_namespace->is_lexical() - is
    // never true there, and a pass that followed it would put every later type in the file into `x`
    // while the two passes after it kept them at the file's
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function f() : void { namespace x; }\n"
        "struct Q { int32 $v; }\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(count_issues_containing(*bundle, "cannot appear inside a body") == 1);

    auto &m = bundle->modules.find_module("test");
    REQUIRE(EchoTests::types_named(m, "Q").size() == 1);

    auto &namespaces = bundle->collector.namespaces;
    REQUIRE(namespaces.find_symbol("Q", namespaces.root()) != nullptr);
    REQUIRE_FALSE(namespaces.exists("x"));
}

TEST_CASE("a body-local struct is refused where a type parameter is visible", "[lexical][generics]")
{
    // the third case of the rule a nested `function` and a closure already obey: `T` resolves through
    // the type-param scope stack, and nothing would ever hand this declaration a substitution for it.
    // A type is the case where accepting it was *silent* - the monomorphizer clones a generic body once
    // per instantiation, and TypeDeclNode::clone had to mint a substituted layout of its own, so one
    // type ended up with two unequal ComplexType* identities
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer<T>(T $v) : int32 {\n"
        "    struct P { int32 $x; }\n"
        "    return 1;\n"
        "}\n"
        "echo outer<int32>(1);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(has_issue_containing(*bundle, "cannot be declared inside a generic function's body"));
}

// -- more of the surface -----------------------------------------------------

TEST_CASE("a function declared in a destructor body resolves", "[lexical]")
{
    // the destructor arm clears SelfScope around its body, so a `function` in there is a scoped free
    // function - and the declaration pass has to open the same frame or the two disagree
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct B {\n"
        "    int32 $v;\n"
        "    destructor() { function bye() : int32 { return 9; } echo bye(); }\n"
        "}\n"
        "function s() : void { B $b = B(1); }\n"
        "s();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto byes = decls_named(m, "bye");
    REQUIRE(byes.size() == 1);
    REQUIRE(byes[0]->owner_type == nullptr);
    REQUIRE(is_file_root_child(m, byes[0]));
}

TEST_CASE("blocks nested three deep each get their own scope", "[lexical]")
{
    // block scoped means *every* block, not only a function body. each `h` is a distinct declaration and
    // the innermost one visible at each point wins
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    { function h() : int32 { return 1; }\n"
        "      { function h() : int32 { return 2; }\n"
        "        { function h() : int32 { return 3; } echo h(); }\n"
        "        echo h(); }\n"
        "      echo h(); }\n"
        "    return 0;\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "h");
    REQUIRE(decls.size() == 3);

    auto calls = calls_to(m, "h");
    REQUIRE(calls.size() == 3);

    // innermost first, in source order: 3, 2, 1
    REQUIRE(calls[0]->decl == decls[2]);
    REQUIRE(calls[1]->decl == decls[1]);
    REQUIRE(calls[2]->decl == decls[0]);

    // and all three mangle apart, which is the half a shared display name would have hidden
    REQUIRE(decls[0]->decorated_func_name() != decls[1]->decorated_func_name());
    REQUIRE(decls[1]->decorated_func_name() != decls[2]->decorated_func_name());
    REQUIRE(decls[0]->decorated_func_name() != decls[2]->decorated_func_name());
}

TEST_CASE("a block holds a whole overload set, not one declaration", "[lexical]")
{
    // the lexical namespace keys (namespace, name) to a *set*, exactly as a written one does - so
    // block-local overloads resolve by argument type like any others
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer() : int32 {\n"
        "    function h(int32 $a) : int32 { return 1; }\n"
        "    function h(float64 $a) : int32 { return 2; }\n"
        "    return h(1) + h(1.5);\n"
        "}\n"
        "echo outer();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "h");
    REQUIRE(decls.size() == 2);

    auto calls = calls_to(m, "h");
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0]->decl != calls[1]->decl);
}

TEST_CASE("a block-local declaration keeps its file's namespace", "[lexical]")
{
    // the lexical namespace is a *child* of the written one, so the namespace path survives - it is the
    // reason `overloads()` still walks outward to reach anything declared at file scope
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "namespace geo;\n"
        "function make() : int32 {\n"
        "    function dbl(int32 $a) : int32 { return $a * 2; }\n"
        "    return dbl(21);\n"
        "}\n"
        "echo make();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto dbls = decls_named(m, "dbl");
    REQUIRE(dbls.size() == 1);

    REQUIRE(dbls[0]->namespaced_func_name() == "geo::make::dbl");
    REQUIRE(dbls[0]->ast_namespace->is_lexical());
    REQUIRE(dbls[0]->ast_namespace->declaring_namespace()->full_name() == "geo");
}

TEST_CASE("a refused nested declaration reports once and recovers", "[lexical]")
{
    // the recovery has to skip the whole declaration, brace-aware. skipping to the next `;` resumed
    // *inside* the body, which reported a cascade about half a body - and for a closure inside a
    // `return` ran the cursor off the end of the file
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function outer<T>(T $v) : int32 {\n"
        "    function helper(int32 $a) : int32 { return $a; }\n"
        "    return 1;\n"
        "}\n"
        "echo outer<int32>(1);\n");

    REQUIRE(bundle->collector.has_critical_issues());
    REQUIRE(count_issues_containing(*bundle, "cannot be declared inside a generic function's body") == 1);

    // exactly one issue in total: nothing downstream saw half a parsed body
    REQUIRE(bundle->collector.issues.size() == 1);
}
