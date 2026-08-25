#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTClone.h>
#include <AST/ASTConstFold.h>
#include <AST/ASTRecursiveVisitor.h>
#include <AST/ASTValueType.h>
#include <AST/ConstIfNode.h>
#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/IfStatementNode.h>
#include <AST/ScopeNode.h>
#include <AST/VarDeclNode.h>
#include <Compiler/TargetFacts.h>
#include <Parser/ModuleParser.h>

#include "helpers.h"

using namespace AST;

// AST::const_fold is the sole answer to "what does this expression fold to before codegen", and it is
// asked from two places that cannot both be exercised from the e2e corpus: a fixpoint round, where a
// not-yet must not read as a refusal, and ExprCodegen, where a not-yet is a compiler bug. so the three
// results are pinned here, in isolation, rather than inferred from a program's output.
//
// every case reaches the folder through a real parse: the arms read result_type(), and a hand-built node
// would be asserting about a type nothing had reconciled.
//
// **the builtins are declared in each case rather than imported**, because these bundles carry no stdlib
// - which is the better test anyway: what the folder reads is `#[builtin:]` and AST::builtin_foldability,
// never a name spelled `mem::`

namespace
{
    // **a comparison is tested in condition position, not as an initializer.** a destination that
    // can_type_a_literal accepts *retypes* the literals in it, so `bool $x = 3 < 4;` parses as two bool
    // literals over the tokens `3` and `4` - which is a defect of its own and not this folder's business.
    // a condition has no destination type, and it is also the position `const if` will read
    ConstFoldResult fold_last_condition(Bundle &bundle)
    {
        ExprNode *found = nullptr;

        for (auto &module_ptr : bundle.modules) {
            for (auto *branch : module_ptr->nodes.of_type<IfStatementNode>()) {
                if (branch->condition != nullptr) {
                    found = branch->condition;
                }
            }
        }

        REQUIRE(found != nullptr);

        return const_fold(found);
    }

    // the initializer of the declaration named `$subject`, which is where a value case writes its
    // expression. by name rather than by position, so a case may declare whatever it needs above it
    ConstFoldResult fold_subject(Bundle &bundle)
    {
        ExprNode *found = nullptr;

        for (auto &module_ptr : bundle.modules) {
            for (auto *decl : module_ptr->nodes.of_type<VarDeclNode>()) {
                if (decl->init_expr != nullptr && decl->token_varname.value() == "$subject") {
                    found = decl->init_expr;
                }
            }
        }

        REQUIRE(found != nullptr);

        return const_fold(found);
    }

    ConstFoldResult fold_value(const std::string &type, const std::string &expr)
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(type + " $subject = " + expr + ";\n");
        return fold_subject(*bundle);
    }

    ConstFoldResult fold_condition(const std::string &expr)
    {
        auto bundle = EchoTests::tests_make_parsed_bundle("if (" + expr + ") { echo 1; }\n");
        return fold_last_condition(*bundle);
    }
}

TEST_CASE("a bool literal folds to itself", "[const_fold]")
{
    const ConstFoldResult yes = fold_condition("true");

    REQUIRE(yes.is_bool());
    REQUIRE(yes.as_bool());

    REQUIRE(fold_condition("false").is_bool());
    REQUIRE_FALSE(fold_condition("false").as_bool());
}

TEST_CASE("an integer literal folds at its declared width", "[const_fold]")
{
    const ConstFoldResult folded = fold_value("int32", "7");

    REQUIRE(folded.is_folded());
    REQUIRE(folded.bits == 7);
    REQUIRE(folded.type == EchoTests::prim(ValueTypePrimitive::t_int32));
}

TEST_CASE("a negative literal folds sign-extended, and its width is checked", "[const_fold]")
{
    // ConstFoldResult::bits documents that a signed value is stored sign-extended to 64 bits, which is
    // what makes as_signed() a cast rather than a shift - and what makes two values of one type
    // comparable by comparing the field
    const ConstFoldResult folded = fold_value("int8", "-128");

    REQUIRE(folded.is_folded());
    REQUIRE(folded.as_signed() == -128);

    // the positive half is one smaller than the negative one, so -128 fitting while 128 would not is the
    // asymmetry a single bound would have got wrong. `int8 $x = -129;` never reaches the folder at all -
    // the parser's own precision check refuses it first - which is why this asserts the boundary that
    // does arrive rather than the one past it
    REQUIRE(fold_value("int8", "127").is_folded());
    REQUIRE(fold_value("int8", "-1").as_signed() == -1);
}

TEST_CASE("a cast folds only when it leaves the value alone", "[const_fold]")
{
    // the widening a reconciliation inserts is folded through, because it cannot disagree with what
    // codegen emits; a conversion that would change the value is refused rather than reproduced
    REQUIRE(fold_value("int64", "7").is_folded());
    REQUIRE(fold_value("float64", "1.5").result == ConstFoldResult::Result::t_refused);
}

TEST_CASE("a literal too wide for the type the parser gave it is refused", "[const_fold]")
{
    // in a position with no destination to type it, a literal is int32 unless it needs int64 - and
    // nothing types one as unsigned, so a value above the int64 maximum has no type that holds it. the
    // folder refuses rather than reinterpreting, which would answer -1
    auto bundle = EchoTests::tests_make_parsed_bundle("if (18446744073709551615 > 1) { echo 1; }\n");

    REQUIRE(fold_last_condition(*bundle).result == ConstFoldResult::Result::t_refused);
}

TEST_CASE("the widest unsigned literal survives being carried", "[const_fold]")
{
    // the reason bits is unsigned storage: a uint64 above INT64_MAX is a legal literal, and reading it
    // back through a signed field would answer -1
    const ConstFoldResult folded = fold_value("uint64", "18446744073709551615");

    REQUIRE(folded.is_folded());
    REQUIRE(folded.bits == 18446744073709551615ULL);
}

TEST_CASE("arithmetic folds, and an overflow is refused rather than wrapped", "[const_fold]")
{
    REQUIRE(fold_value("int32", "4 * 8").bits == 32);
    REQUIRE(fold_value("int32", "10 - 3").bits == 7);
    REQUIRE(fold_value("int32", "10 / 3").bits == 3);
    REQUIRE(fold_value("int32", "10 % 3").bits == 1);

    // `const(255 + 1)` at uint8 wraps to 0 at runtime and is refused here, deliberately - a `const`
    // value the compiler stands behind must not be a value nobody asked for
    REQUIRE(fold_value("uint8", "255 + 1").result == ConstFoldResult::Result::t_refused);
    REQUIRE(fold_value("uint8", "0 - 1").result == ConstFoldResult::Result::t_refused);
    REQUIRE(fold_value("int32", "1 / 0").result == ConstFoldResult::Result::t_refused);
    REQUIRE(fold_value("int32", "1 % 0").result == ConstFoldResult::Result::t_refused);
}

TEST_CASE("`**` is not folded, because codegen would not agree", "[const_fold]")
{
    // it round-trips through double and llvm.pow, so an integer answer here would differ silently for
    // any input where that loses precision
    REQUIRE(fold_value("int32", "2 ** 3").result == ConstFoldResult::Result::t_refused);
}

TEST_CASE("a comparison folds, and reads the operand type's own signedness", "[const_fold]")
{
    REQUIRE(fold_condition("3 < 4").as_bool());
    REQUIRE_FALSE(fold_condition("4 < 3").as_bool());
    REQUIRE(fold_condition("4 == 4").as_bool());
    REQUIRE_FALSE(fold_condition("4 != 4").as_bool());
    REQUIRE(fold_condition("4 >= 4").as_bool());
    REQUIRE(fold_condition("5 > 4").as_bool());
    REQUIRE(fold_condition("-1 < 1").as_bool());
}

TEST_CASE("two uint8s compare unsigned", "[const_fold]")
{
    // variables do not fold; the constants expand to uint8 literals, which is the pair the folder
    // and codegen share binary_operation_type's signedness for. 200 > 57 is true unsigned and
    // false as signed i8
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "const uint8 B = 200;\n"
        "const uint8 HI = 57;\n"
        "if (B > HI) { echo 1; }\n");

    REQUIRE(fold_last_condition(*bundle).is_bool());
    REQUIRE(fold_last_condition(*bundle).as_bool());
}

TEST_CASE("mismatched operands are reconciled through common_numeric_type", "[const_fold]")
{
    // **the shape a constant creates.** AST::ConstantExpander lands its clone *after* the parser has
    // reconciled what it could see, so a `usize` constant beside an `int32` literal arrives here with
    // nothing having agreed on a type. asking AST::common_numeric_type is what keeps that one rule in
    // one place instead of guessing which side wins
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "const usize LIMIT = 100;\n"
        "if (LIMIT > 50) { echo 1; }\n");

    const ConstFoldResult folded = fold_last_condition(*bundle);

    REQUIRE(folded.is_bool());
    REQUIRE(folded.as_bool());

    // and the unsigned ordering path it reaches, which ExprCodegen's integer arm answers the same way:
    // both read the signedness of the type this reconciliation named
    auto wide = EchoTests::tests_make_parsed_bundle(
        "const uint64 HUGE = 18446744073709551615;\n"
        "if (HUGE > 1) { echo 1; }\n");

    REQUIRE(fold_last_condition(*wide).as_bool());
}

TEST_CASE("`&&` and `||` fold, and short-circuit", "[const_fold]")
{
    REQUIRE(fold_condition("true && true").as_bool());
    REQUIRE_FALSE(fold_condition("true && false").as_bool());
    REQUIRE(fold_condition("false || true").as_bool());
    REQUIRE_FALSE(fold_condition("false || false").as_bool());
    REQUIRE(fold_condition("3 < 4 && 5 > 4").as_bool());

    // **the right side is not required to fold** once the left has decided. a call is refused as
    // "only known when it runs", so `false && side()` must not become that refusal
    auto short_and = EchoTests::tests_make_parsed_bundle(
        "function side() : bool { return true; }\n"
        "if (false && side()) { echo 1; }\n");
    const ConstFoldResult and_folded = fold_last_condition(*short_and);
    REQUIRE(and_folded.is_bool());
    REQUIRE_FALSE(and_folded.as_bool());

    auto short_or = EchoTests::tests_make_parsed_bundle(
        "function side() : bool { return false; }\n"
        "if (true || side()) { echo 1; }\n");
    const ConstFoldResult or_folded = fold_last_condition(*short_or);
    REQUIRE(or_folded.is_bool());
    REQUIRE(or_folded.as_bool());

    // the other way round still needs the right side
    REQUIRE(fold_condition("true && (3 < 4)").as_bool());
    REQUIRE_FALSE(fold_condition("false || (4 < 3)").as_bool());
}

TEST_CASE("the two ownership builtins fold from the taxonomy", "[const_fold]")
{
    // an int32 owns nothing, so it copies as bytes and needs no teardown
    auto trivial = EchoTests::tests_make_parsed_bundle(
        "#[builtin: is_trivially_copyable]\n"
        "function itc<T>() : bool;\n"
        "if (itc<int32>()) { echo 1; }\n");

    REQUIRE(fold_last_condition(*trivial).as_bool());

    // and a class handle owns a reference, so both answers invert - because AST::classify_copy and
    // AST::needs_destruction say so, not because this file does
    auto owning = EchoTests::tests_make_parsed_bundle(
        "#[builtin: needs_destruction]\n"
        "function nd<T>() : bool;\n"
        "class Node { int32 $v; }\n"
        "if (nd<Node>()) { echo 1; }\n");

    REQUIRE(fold_last_condition(*owning).as_bool());

    auto plain = EchoTests::tests_make_parsed_bundle(
        "#[builtin: needs_destruction]\n"
        "function nd<T>() : bool;\n"
        "if (nd<int32>()) { echo 1; }\n");

    REQUIRE(fold_last_condition(*plain).is_bool());
    REQUIRE_FALSE(fold_last_condition(*plain).as_bool());
}

TEST_CASE("a builtin with a concrete explicit type argument folds while still generic", "[const_fold]")
{
    // **the clone-time const if path.** ConstIfNode::clone folds the substituted condition before
    // instantiate_generic_calls rewires the builtin, so the folder must accept a still-generic
    // decl whose explicit type argument is already concrete. ConstFolding is still the splicer
    auto bundle = std::make_unique<Bundle>();
    auto handle = bundle->modules.add_module("test");
    Parser::ModuleParser::InputPayload input {
        .files = {
            Parser::ModuleParser::InputFile(
                "/tmp/testfile.eco",
                "#[builtin: needs_destruction]\n"
                "function nd<T>() : bool;\n"
                "if (nd<int32>()) { echo 1; }\n")
        },
        .module = bundle->modules.get_module(handle),
        .collector = bundle->collector
    };
    Parser::ModuleParser(Compiler::TargetFacts::host()).parse_input(input);

    REQUIRE(fold_last_condition(*bundle).is_bool());
    REQUIRE_FALSE(fold_last_condition(*bundle).as_bool());

    auto calls = EchoTests::calls_to(bundle->modules.find_module("test"), "nd");
    REQUIRE_FALSE(calls.empty());
    REQUIRE(calls[0]->decl != nullptr);
    REQUIRE(calls[0]->decl->is_generic());
}

TEST_CASE("clone of a folded const if stays a ConstIfNode and drops the dead arm", "[const_fold][clone]")
{
    // ConstFolding is the splicer. clone is type-preserving: it folds the substituted condition
    // and leaves the other arm null, so the discarded subtree is never in the instance
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[builtin: needs_destruction]\n"
        "function nd<T>() : bool;\n"
        "function choose<T>() : int32 {\n"
        "    const if (nd<T>()) {\n"
        "        return 1;\n"
        "    } else {\n"
        "        return 0;\n"
        "    }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    FunctionDeclNode *tmpl = nullptr;

    for (auto *decl : EchoTests::decls_named(m, "choose")) {
        if (decl->is_generic()) {
            tmpl = decl;
        }
    }

    REQUIRE(tmpl != nullptr);
    REQUIRE_FALSE(tmpl->type_parameters.empty());

    TypeSubstitution subst = TypeSubstitution::positional(
        tmpl->type_parameters, {ValueType(ValueTypePrimitive::t_int32)});
    CloneContext cc(m.nodes, subst, bundle->collector.type_registry);

    auto *cloned = static_cast<FunctionDeclNode *>(tmpl->clone(cc));
    REQUIRE(cloned != nullptr);
    REQUIRE(cloned->body != nullptr);

    ConstIfNode *branch = nullptr;

    for (auto &child : cloned->body->children) {
        if (child.has_type<ConstIfNode>()) {
            branch = child.get_ptr<ConstIfNode>();
        }
    }

    REQUIRE(branch != nullptr);
    REQUIRE(branch->if_scope == nullptr);
    REQUIRE(branch->else_scope != nullptr);
}

TEST_CASE("clone of a generic body keeps only the taken const if arm", "[const_fold][clone]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[builtin: needs_destruction]\n"
        "function nd<T>() : bool;\n"
        "function poison<T>() : void {}\n"
        "function choose<T>() : int32 {\n"
        "    const if (nd<T>()) {\n"
        "        poison<T>();\n"
        "        return 1;\n"
        "    } else {\n"
        "        return 0;\n"
        "    }\n"
        "}\n"
        "echo choose<int32>();\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    FunctionDeclNode *instance = nullptr;

    for (auto *decl : EchoTests::decls_named(m, "choose")) {
        if (!decl->is_generic()) {
            instance = decl;
        }
    }

    REQUIRE(instance != nullptr);
    REQUIRE(instance->body != nullptr);

    class FindsConstIf : public RecursiveVisitor
    {
    public:
        bool found = false;

        void visit_const_if(ConstIfNode &) override {
            found = true;
        }

        void visitFunctionDecl(FunctionDeclNode &) override {}
    };

    FindsConstIf find;
    instance->body->accept(find);
    REQUIRE_FALSE(find.found);

    bool poison_instantiated = false;

    for (auto *decl : EchoTests::decls_named(m, "poison")) {
        if (!decl->is_generic()) {
            poison_instantiated = true;
        }
    }

    REQUIRE_FALSE(poison_instantiated);
}

TEST_CASE("a builtin asked about an unbound T is pending, never a refusal", "[const_fold]")
{
    // **the distinction the three-result shape exists for.** AST::classify_copy answers "no" for an
    // unsettled type parameter on purpose, so folding against an unbound `T` is silently the wrong
    // answer in the one direction that compiles. inside a template body it has to read as a not-yet
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[builtin: needs_destruction]\n"
        "function nd<T>() : bool;\n"
        "function probe<T>() : int32 { if (nd<T>()) { return 1; } return 0; }\n");

    REQUIRE(fold_last_condition(*bundle).result == ConstFoldResult::Result::t_pending);
}

TEST_CASE("a layout query is refused, and says why", "[const_fold]")
{
    // size_of and align_of read an llvm::DataLayout, which exists only at codegen. a refusal rather than
    // a pending, because no fixpoint round is ever going to answer it
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[builtin: size_of]\n"
        "function so<T>() : usize;\n"
        "usize $subject = so<int32>();\n");

    const ConstFoldResult folded = fold_subject(*bundle);

    REQUIRE(folded.result == ConstFoldResult::Result::t_refused);
    REQUIRE(folded.refusal.find("layout") != std::string::npos);
}

TEST_CASE("a builtin that does something rather than answering is refused", "[const_fold]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "#[builtin: live_allocations]\n"
        "function live() : usize;\n"
        "usize $subject = live();\n");

    REQUIRE(fold_subject(*bundle).result == ConstFoldResult::Result::t_refused);
}

TEST_CASE("a variable and an ordinary call are refused", "[const_fold]")
{
    auto with_var = EchoTests::tests_make_parsed_bundle(
        "int32 $n = 3;\n"
        "int32 $subject = $n;\n");

    REQUIRE(fold_subject(*with_var).result == ConstFoldResult::Result::t_refused);

    auto with_call = EchoTests::tests_make_parsed_bundle(
        "function three() : int32 { return 3; }\n"
        "int32 $subject = three();\n");

    REQUIRE(fold_subject(*with_call).result == ConstFoldResult::Result::t_refused);
}

TEST_CASE("a declared operator runs rather than folding", "[const_fold]")
{
    // the gate is AST::binary_has_builtin_meaning, which answers false for a custom symbol and for any
    // complex operand - so this reaches the refusal instead of being folded on operand bits, which for
    // a struct would be folding something that is not even a number
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point { int32 $x; }\n"
        "operator (Point $a) < (Point $b) : bool { return $a->x < $b->x; }\n"
        "if (Point(1) < Point(2)) { echo 1; }\n");

    REQUIRE(fold_last_condition(*bundle).result == ConstFoldResult::Result::t_refused);
}

TEST_CASE("a float is refused", "[const_fold]")
{
    // owning a float comparison means owning rounding and the target's precision, which ExprCodegen does
    REQUIRE(fold_value("float64", "1.5").result == ConstFoldResult::Result::t_refused);
}
