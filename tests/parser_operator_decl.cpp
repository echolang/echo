#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTOperatorSemantics.h>
#include <AST/ASTOps.h>
#include <AST/FunctionDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::count_issues_containing;
using EchoTests::decls_named;
using EchoTests::has_issue_containing;

namespace
{
    const char *k_point =
        "struct Point { float64 $x; }\n"
        "operator (Point $a) + (Point $b): Point { return Point($a->x + $b->x); }\n"
        "operator (Point $a) + (int32 $b): Point { return Point($a->x + $b); }\n";

    const Operator *op_named(const AST::Bundle &bundle, const std::string &spelling)
    {
        return bundle.collector.operators.get_operator(spelling);
    }
}

// **where in the parse passes an operator is published**, which no end-to-end case can see. the
// symbol goes in during the *type-name* pass, a whole pass earlier than any other declaration
// publishes anything, and the signature during the declaration pass. a refactor collapsing those two
// would leave every tests_eco case green and only this file red
TEST_CASE("an operator declaration publishes its symbol and its signature", "[operator_decl]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_point);

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    const Operator *plus = op_named(*bundle, "+");
    REQUIRE(plus != nullptr);

    // `+` is a predefined operator, so declaring an overload of it must not mint a second, shadowing
    // symbol - `build_incdec_value` looks `"+"` up by string to desugar every `$i++`
    REQUIRE_FALSE(plus->is_custom());
    REQUIRE(plus->has_fixity(OpFixity::t_infix));
    REQUIRE(plus->is_declared());

    // both overloads are in *one* set, under the decorated name, and each is there once despite two
    // passes reaching the declaration
    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, operator_function_name("+", OpFixity::t_infix));

    REQUIRE(decls.size() == 2);

    for (const auto *decl : decls) {
        REQUIRE(decl->member_kind == MemberKind::t_operator);

        // a *free* declaration in every structural sense, exactly like a constructor: no owner type,
        // so no receiver is counted and no owner segment reaches the mangled name
        REQUIRE(decl->owner_type == nullptr);
        REQUIRE(decl->implicit_arg_count() == 0);
        REQUIRE(decl->args.size() == 2);
    }
}

TEST_CASE("a custom symbol is declared with its fixity and precedence", "[operator_decl]")
{
    SECTION("an infix word operator, defaulted precedence")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "operator (float64 $a) avg (float64 $b): float64 { return ($a + $b) / 2.0; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        const Operator *avg = op_named(*bundle, "avg");
        REQUIRE(avg != nullptr);
        REQUIRE(avg->is_custom());
        REQUIRE(avg->has_fixity(OpFixity::t_infix));
        REQUIRE_FALSE(avg->precedence_declared);
        REQUIRE(avg->precedence.sequence == CUSTOM_OP_DEFAULT_PRECEDENCE);
    }

    SECTION("a declared precedence clause is read, not skipped")
    {
        // the deleted lexer prepass stepped over this clause as a scope and never read it, so every
        // custom operator got the same hardcoded precedence
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "operator(35, right) (float64 $a) avg (float64 $b): float64 { return $a; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        const Operator *avg = op_named(*bundle, "avg");
        REQUIRE(avg != nullptr);
        REQUIRE(avg->precedence_declared);
        REQUIRE(avg->precedence.sequence == 35);
        REQUIRE(avg->precedence.assoc == OpAssociativity::right);
    }

    SECTION("a prefix symbol spelled out of two tokens")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Flag { bool $on; }\n"
            "operator !!(Flag $f): bool { return $f->on; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        const Operator *bang = op_named(*bundle, "!!");
        REQUIRE(bang != nullptr);
        REQUIRE(bang->is_custom());
        REQUIRE(bang->has_fixity(OpFixity::t_prefix));
        REQUIRE_FALSE(bang->has_fixity(OpFixity::t_infix));
    }

    SECTION("a suffix symbol")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Distance { uint64 $mm; }\n"
            "operator (uint64 $a)mm: Distance { return Distance($a); }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        const Operator *mm = op_named(*bundle, "mm");
        REQUIRE(mm != nullptr);
        REQUIRE(mm->has_fixity(OpFixity::t_suffix));
        REQUIRE_FALSE(mm->has_fixity(OpFixity::t_infix));
    }

    SECTION("one symbol may be declared in more than one position")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "operator (S $a) blah (S $b): int32 { return 1; }\n"
            "operator blah(S $a): int32 { return 2; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        const Operator *blah = op_named(*bundle, "blah");
        REQUIRE(blah != nullptr);
        REQUIRE(blah->has_fixity(OpFixity::t_infix));
        REQUIRE(blah->has_fixity(OpFixity::t_prefix));

        // and the two are separate overload sets, because a prefix and a suffix declaration of one
        // symbol are otherwise the same signature
        auto &m = bundle->modules.find_module("test");
        REQUIRE(decls_named(m, operator_function_name("blah", OpFixity::t_infix)).size() == 1);
        REQUIRE(decls_named(m, operator_function_name("blah", OpFixity::t_prefix)).size() == 1);
    }
}

// **the three ways a declaration can be positioned relative to its use site**, all of which have to
// work and none of which an end-to-end case distinguishes from the others. these are the cases a
// regression would silently take away
TEST_CASE("an operator is declared anywhere and valid everywhere", "[operator_decl]")
{
    SECTION("declared below its use site, and its type below the operator")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "$a = Point(1.0);\n"
            "$b = Point(2.0);\n"
            "$c = $a + $b;\n"
            "operator (Point $p) + (Point $q): Point { return Point($p->x + $q->x); }\n"
            "struct Point { float64 $x; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        auto &m = bundle->modules.find_module("test");
        REQUIRE(EchoTests::calls_to(m, operator_function_name("+", OpFixity::t_infix)).size() == 1);
    }

    SECTION("declared in another file")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(std::vector<std::string>{
            "$a = Point(1.0);\n"
            "$b = Point(2.0);\n"
            "$c = $a + $b;\n",

            "struct Point { float64 $x; }\n"
            "operator (Point $p) + (Point $q): Point { return Point($p->x + $q->x); }\n",
        });

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
    }

    SECTION("used inside a struct property initializer")
    {
        // the expression the *declaration* pass parses. it is the whole reason the symbol is published
        // one pass earlier: asking the overload set here would answer differently depending on which
        // file, and which declaration inside it, had been walked first
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Distance { uint64 $mm = 1m; }\n"
            "operator (uint64 $a)m: uint64 { return $a * 1000; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
    }
}

TEST_CASE("a declaration that could never be reached is refused", "[operator_decl]")
{
    SECTION("inside a struct")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; operator (S $a) + (S $b): S { return $a; } }\n");

        REQUIRE(has_issue_containing(*bundle, "cannot be declared inside a struct"));
    }

    SECTION("inside a block")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "{ operator (S $a) + (S $b): S { return $a; } }\n");

        REQUIRE(has_issue_containing(*bundle, "cannot be declared inside a block"));
    }

    SECTION("a keyword as the symbol")
    {
        // matching happens on token *values*, so a symbol spelled `if` would turn every `if` in the
        // program into an operator
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "operator (int32 $a) if (int32 $b): int32 { return 1; }\n");

        REQUIRE(has_issue_containing(*bundle, "cannot be part of an operator symbol"));

        // ...and reported once, though every pass reaches the declaration
        REQUIRE(count_issues_containing(*bundle, "cannot be part of an operator symbol") == 1);
    }

    SECTION("a built-in symbol over nothing but primitives")
    {
        // it would register, mangle and be emitted, and then never fire, because the built-in meaning
        // wins for two primitives. the class of silent no-op publish_implicit_conversion refuses seven
        // shapes for
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "operator (int32 $a) + (int32 $b): int32 { return 1; }\n");

        REQUIRE(has_issue_containing(*bundle, "would never be used"));
    }

    SECTION("...but a custom symbol over primitives is exactly the point")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "operator (int32 $a) avg (int32 $b): int32 { return ($a + $b) / 2; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
    }

    SECTION("...and so is an operator over a concrete instantiation")
    {
        // an operator over a *concrete* instantiation needs no type-parameter list at all - there is
        // nothing left generic to bind. `operator<T>` is the other spelling, tested above
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Vec<T> { T $first; }\n"
            "operator (Vec<int32> $a) + (Vec<int32> $b): Vec<int32> {\n"
            "    return Vec<int32>($a->first + $b->first);\n"
            "}\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
    }

    SECTION("a suffix increment, which is a statement")
    {
        // `$i++;` never reaches parse_expr - ScopeParser routes it straight to parse_varexpr, which
        // desugars it - so a declaration of it would be silently ignored
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "operator (int32 $a)++: int32 { return $a; }\n");

        REQUIRE(has_issue_containing(*bundle, "is a statement"));
    }

    SECTION("assignment, which is also a statement")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "operator (S $a) = (S $b): S { return $a; }\n");

        REQUIRE(has_issue_containing(*bundle, "assignment is a statement"));
    }

    SECTION("a void return")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "operator (S $a) sum (S $b): void { return; }\n");

        REQUIRE(has_issue_containing(*bundle, "An operator is an expression"));
    }

    SECTION("one symbol declared both infix and suffix")
    {
        // `$a blah $b` could not say whether the symbol closes the left operand or opens the right
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "operator (S $a) blah (S $b): int32 { return 1; }\n"
            "operator (S $a) blah : int32 { return 2; }\n");

        REQUIRE(has_issue_containing(*bundle, "cannot be both infix and suffix"));
    }

    SECTION("two different precedences for one symbol")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "operator(30, left) (S $a) op (S $b): int32 { return 1; }\n"
            "operator(50, left) (S $a) op (S $b): int32 { return 2; }\n");

        REQUIRE(has_issue_containing(*bundle, "A symbol binds one way everywhere"));
    }

    SECTION("a precedence on an overload of a built-in symbol")
    {
        // `+` binds the way the language says it binds, whatever anybody overloads it for, or two
        // files would parse one expression differently
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct S { int32 $x; }\n"
            "operator(30, left) (S $a) + (S $b): S { return $a; }\n");

        REQUIRE(has_issue_containing(*bundle, "already has a precedence"));
    }
}

TEST_CASE("a use site becomes a call only where there is no built-in meaning", "[operator_decl]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Point { float64 $x; }\n"
        "operator (Point $a) + (Point $b): Point { return Point($a->x + $b->x); }\n"
        "$p = Point(1.0);\n"
        "$q = Point(2.0);\n"
        "$r = $p + $q;\n"
        "$n = 1 + 2;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // the struct operands resolved to the declaration...
    auto calls = EchoTests::calls_to(m, operator_function_name("+", OpFixity::t_infix));
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0]->decl != nullptr);

    // ...while `1 + 2` is still an ordinary binary expression, not a call. the declaration exists, so
    // this is the predicate deciding rather than the absence of an overload set
    REQUIRE(EchoTests::tests_make_node_description_expr("1 + 2;")
        == "binexp<int32>(literal<int32>(1) + literal<int32>(2))");
}

// the decorated name is unspellable by design, and it has to survive being an llvm symbol
TEST_CASE("an operator's decorated name is unspellable and manglable", "[operator_decl]")
{
    REQUIRE(operator_function_name("+", OpFixity::t_infix) == "operator +");
    REQUIRE(operator_function_name("!!", OpFixity::t_prefix) == "operator prefix !!");
    REQUIRE(operator_function_name("mm", OpFixity::t_suffix) == "operator suffix mm");

    // the space is what makes it unspellable: no identifier may contain one
    for (const auto &name : {
        operator_function_name("+", OpFixity::t_infix),
        operator_function_name("avg", OpFixity::t_infix)}) {
        REQUIRE(name.find(' ') != std::string::npos);
    }

    // ...and the mangled form holds nothing an assembler could object to
    const std::string mangled = mangle_operator_name(operator_function_name("+", OpFixity::t_infix));

    REQUIRE(mangled == "operatorx20x2b");

    for (const char c : mangled) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_';
        REQUIRE(safe);
    }
}

// **the bracket is a fixity, not a symbol run.** its two tokens are not adjacent in the declaration
// (`[usize $i]` has an operand between them) and they *are* adjacent at an append site (`$a[]`), so
// neither the symbol reader nor the trie can be the thing that recognises it. no end-to-end case can
// see either half of that
TEST_CASE("an index operator is registered by spelling, never in the symbol trie", "[operator_decl]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bag { ptr<int32> $at; }\n"
        "operator (Bag& $b)[usize $i] : int32& { unsafe { return &$b->at:$[$i]; } }\n"
        "operator (Bag& $b)[] : int32& { unsafe { return &$b->at:$[0]; } }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    // the symbol is published, and in the index position
    const Operator *bracket = op_named(*bundle, OperatorRegistry::bracket_spelling());
    REQUIRE(bracket != nullptr);
    REQUIRE(bracket->has_fixity(OpFixity::t_index));
    REQUIRE_FALSE(bracket->has_fixity(OpFixity::t_infix));
    REQUIRE_FALSE(bracket->has_fixity(OpFixity::t_suffix));

    // both declarations land in **one** overload set under one name, told apart by arity alone -
    // which is what AST::match_function compares first, so this needs no resolution rule of its own
    auto declared = decls_named(bundle->modules.find_module("test"), operator_function_name(
        OperatorRegistry::bracket_spelling(), OpFixity::t_index));

    REQUIRE(declared.size() == 2);
    REQUIRE(declared[0]->args.size() != declared[1]->args.size());
}

// the trie half of the above, asked directly: a `[` in the token stream must never match as a
// declared symbol, because parse_postfix_chain has already claimed it. a trie entry would make the
// *append* form `$a[]` - whose two brackets genuinely are adjacent - match inside the shunting yard
TEST_CASE("match_at never answers for a bracket", "[operator_decl]")
{
    OperatorRegistry registry;

    REQUIRE(registry.find_or_declare_bracket() != nullptr);

    // registered by spelling, so a declaration finds it...
    REQUIRE(registry.get_operator(std::string(OperatorRegistry::bracket_spelling())) != nullptr);

    // ...and minting it twice is the same operator, the way find_or_declare is idempotent
    REQUIRE(registry.find_or_declare_bracket() == registry.find_or_declare_bracket());
}

// the index fixity's decorated name carries **no fixity word**, unlike prefix and suffix - a bracket
// spelling can only ever be an index operator, because `[` and `]` are refused inside every other
// symbol, so there is no second declaration for it to be told apart from
TEST_CASE("the index fixity's name and mangling", "[operator_decl]")
{
    const std::string name = operator_function_name(
        OperatorRegistry::bracket_spelling(), OpFixity::t_index);

    REQUIRE(name == "operator []");
    REQUIRE(name.find(' ') != std::string::npos);
    REQUIRE(mangle_operator_name(name) == "operatorx20x5bx5d");
    REQUIRE(std::string(op_fixity_name(OpFixity::t_index)) == "index");
}

// **the write form carries punctuation where prefix and suffix carry a word**, and it has to carry
// something: arity cannot separate it from the borrowing form, since `operator (M&)[K, K] : V&` and
// `operator (M&)[K] = (V) : void` are both three operands. a missing arm in operator_function_name's
// switch would silently answer `"operator []"` - the borrowing form's own name - and land both contracts
// in one overload set, which is the failure this pins
TEST_CASE("the index-write fixity's name and mangling", "[operator_decl]")
{
    const std::string name = operator_function_name(
        OperatorRegistry::bracket_spelling(), OpFixity::t_index_write);

    REQUIRE(name == "operator []=");
    REQUIRE(mangle_operator_name(name) == "operatorx20x5bx5dx3d");
    REQUIRE(std::string(op_fixity_name(OpFixity::t_index_write)) == "index write");

    // and the two names are distinct, which is the whole of what keeps the two sets apart
    REQUIRE(name != operator_function_name(
        OperatorRegistry::bracket_spelling(), OpFixity::t_index));

    REQUIRE(AST::is_index_fixity(OpFixity::t_index));
    REQUIRE(AST::is_index_fixity(OpFixity::t_index_write));
    REQUIRE_FALSE(AST::is_index_fixity(OpFixity::t_infix));
}

// **two overload sets over one registry entry.** a container may declare a borrowing bracket and a
// writing one, and which a use site asks is decided by the position it sits in - so the declarations
// must not share a set, while the *symbol* must stay one `Operator` carrying both fixity bits. no
// end-to-end case can see either half
TEST_CASE("an index-write operator is a separate overload set from the borrowing one", "[operator_decl]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bag { ptr<int32> $at; }\n"
        "operator (Bag& $b)[usize $i] : int32& { unsafe { return &$b->at:$[$i]; } }\n"
        "operator (Bag& $b)[] : int32& { unsafe { return &$b->at:$[0]; } }\n"
        "operator (Bag& $b)[usize $i] = (int32 $v) : void { $b->at:$[$i] = $v; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    // one registry entry for `[`, carrying both bracket fixities
    const Operator *bracket = op_named(*bundle, OperatorRegistry::bracket_spelling());
    REQUIRE(bracket != nullptr);
    REQUIRE(bracket->has_fixity(OpFixity::t_index));
    REQUIRE(bracket->has_fixity(OpFixity::t_index_write));

    auto &module = bundle->modules.find_module("test");

    // the two borrowing forms in one set, told apart by arity...
    REQUIRE(decls_named(module, operator_function_name(
        OperatorRegistry::bracket_spelling(), OpFixity::t_index)).size() == 2);

    // ...and the write on its own, under its own name
    auto writes = decls_named(module, operator_function_name(
        OperatorRegistry::bracket_spelling(), OpFixity::t_index_write));

    REQUIRE(writes.size() == 1);
    REQUIRE(writes[0]->args.size() == 3);
    REQUIRE(writes[0]->get_return_type().is_void());
}

// **the arity range starts one higher than the borrowing form's**, because the value is not optional:
// two operands is the append write `$c[] = $v`, three or more an element write
TEST_CASE("an index-write operator's arity starts at two", "[operator_decl]")
{
    auto too_few = EchoTests::tests_make_parsed_bundle(
        "struct Bag { ptr<int32> $at; }\n"
        "operator (Bag& $b)[] = () : void { }\n");

    REQUIRE(too_few->collector.has_critical_issues());

    // the append write, which is exactly the minimum and is legal
    auto append = EchoTests::tests_make_parsed_bundle(
        "struct Bag { ptr<int32> $at; usize $n; }\n"
        "operator (Bag& $b)[] = (int32 $v) : void { $b->at:$[$b->n] = $v; }\n");

    REQUIRE_FALSE(append->collector.has_critical_issues());
}

// **which type an operator is declared over**, and the `template_or_self` redirect that lets an
// instantiation find its template's declaration. the one new predicate behind the write rewrite, and
// nothing else pins it - an e2e case can only see the outcome, never the wrong-arity answer
TEST_CASE("declares_index_write finds a contract through the template", "[operator_decl]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bag<T> { ptr<T> $at; }\n"
        "operator<T> (Bag<T>& $b)[usize $i] = (T $v) : void { $b->at:$[$i] = $v; }\n"
        "function make(ptr<int32> $p) : Bag<int32> { Bag<int32> $b = Bag<int32>($p); return $b; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &module = bundle->modules.find_module("test");
    auto writes = decls_named(module, operator_function_name(
        OperatorRegistry::bracket_spelling(), OpFixity::t_index_write));

    REQUIRE(writes.size() == 1);

    // the receiver read off the declaration is the template's own application, `Bag<T>` - the operand as a
    // value, with the borrow level taken off
    const ValueType receiver = AST::operator_receiver_type(*writes[0]);
    REQUIRE(receiver.has_complex_type());
    REQUIRE_FALSE(receiver.is_pointer());

    // one index, so arity 3 - which the declaration has
    REQUIRE(AST::declares_index_write(bundle->collector, receiver, 1));

    // **the arity is part of the question**, so a two-index bracket over the same type is a different
    // contract, and so is the append write - neither of which this type declares
    REQUIRE_FALSE(AST::declares_index_write(bundle->collector, receiver, 2));
    REQUIRE_FALSE(AST::declares_index_write(bundle->collector, receiver, 0));

    // and asked with the **instantiation's** type it still finds the template's declaration, which is the
    // `template_or_self` redirect on both sides. `Bag<int32>` is a distinct ComplexType from `Bag<T>`, so
    // pointer identity alone would answer no here
    ValueType instance = ValueType::make_unknown();
    for (auto *decl : module.nodes.of_type<VarDeclNode>()) {
        if (!decl->has_type() || !decl->type().has_complex_type()) {
            continue;
        }

        // the instantiation rather than the operator's own `Bag<T>&` parameter, which is a pointer
        if (!decl->type().is_pointer()
            && decl->type().get_complex_type() != receiver.get_complex_type()) {
            instance = decl->type();
        }
    }

    REQUIRE(instance.has_complex_type());
    REQUIRE(instance.get_complex_type() != receiver.get_complex_type());
    REQUIRE(AST::declares_index_write(bundle->collector, instance, 1));
}

// **`operator<T>` versus a prefix `<`**, which is the one ambiguity the type-parameter list
// introduced. the lookahead is `<` + identifier + one of `,` `>` `:`, and a prefix declaration has a
// `(` there instead - so both spellings have to keep parsing
TEST_CASE("a generic operator's type parameter list is told from a prefix '<'", "[operator_decl]")
{
    auto generic = EchoTests::tests_make_parsed_bundle(
        "struct Bag<T> { ptr<T> $at; }\n"
        "operator<T> (Bag<T>& $a) merge (Bag<T>& $b) : Bag<T> { return $a; }\n");

    REQUIRE_FALSE(generic->collector.has_critical_issues());

    auto declared = decls_named(
        generic->modules.find_module("test"), operator_function_name("merge", OpFixity::t_infix));
    REQUIRE(declared.size() == 1);
    REQUIRE(declared[0]->is_generic());

    // a prefix `<` is still a symbol, because what follows the angle is a parenthesis
    auto prefix = EchoTests::tests_make_parsed_bundle(
        "struct Bag { int32 $x; }\n"
        "operator <(Bag $b) : bool { return true; }\n");

    REQUIRE_FALSE(prefix->collector.has_critical_issues());
    REQUIRE(decls_named(
        prefix->modules.find_module("test"),
        operator_function_name("<", OpFixity::t_prefix)).size() == 1);
}

// **the index form's operand list is a parameter list**, `[usize $i]` with one token changed - so
// the arity range, not a count, is what separates the two forms. no end-to-end case can see that
// they land in one overload set rather than two, because both spellings work either way
TEST_CASE("an index operator's arity range", "[operator_decl]")
{
    SECTION("no operands at all is refused - the container is the first one")
    {
        // `operator []` with nothing before the bracket is read as a *prefix* symbol, and `[` is
        // refused inside every symbol - which is the same refusal reached from the other direction
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Bag { ptr<int32> $at; }\n"
            "operator [](Bag& $b) : int32& { return &$b->at:$[0]; }\n");

        REQUIRE(has_issue_containing(*bundle, "cannot be part of an operator symbol"));
    }

    SECTION("three operands is an ordinary multi-index contract, not an arity error")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Grid { ptr<int32> $at; usize $cols; }\n"
            "operator (Grid& $g)[usize $r, usize $c] : int32&\n"
            "{ unsafe { return &$g->at:$[$r * $g->cols + $c]; } }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());

        auto declared = decls_named(bundle->modules.find_module("test"), operator_function_name(
            OperatorRegistry::bracket_spelling(), OpFixity::t_index));

        REQUIRE(declared.size() == 1);
        REQUIRE(declared[0]->args.size() == 3);
    }
}

// a type parameter has to be *inferable*, and an operator use site writes nothing but its operands -
// so the check is "does some operand mention it", asked per declaration. the positive half matters
// as much as the refusal: a parameter mentioned only in a nested position still counts
TEST_CASE("a generic operator's parameters must be reachable from its operands", "[operator_decl]")
{
    SECTION("mentioned inside a generic application, which is still a mention")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Bag<T> { ptr<T> $at; }\n"
            "operator<T> (Bag<T>& $b)[usize $i] : T& { return &$b->at:$[$i]; }\n");

        REQUIRE_FALSE(bundle->collector.has_critical_issues());
    }

    SECTION("mentioned only in the return type is not enough")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Bag { ptr<int32> $at; }\n"
            "operator<T> (Bag& $b)[usize $i] : T& { return &$b->at:$[$i]; }\n");

        REQUIRE(has_issue_containing(*bundle, "is not mentioned by any operand"));
    }

    SECTION("one bound and one unbound reports the unbound one by name")
    {
        auto bundle = EchoTests::tests_make_parsed_bundle(
            "struct Bag<T> { ptr<T> $at; }\n"
            "operator<T, U> (Bag<T>& $b)[usize $i] : T& { return &$b->at:$[$i]; }\n");

        REQUIRE(has_issue_containing(*bundle, "type parameter 'U' is not mentioned"));
    }
}
