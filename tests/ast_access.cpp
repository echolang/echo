#include <catch2/catch_test_macros.hpp>

#include <AST/ASTAccess.h>
#include <AST/ASTBundle.h>
#include <AST/ASTMemberLookup.h>
#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::decls_named;
using EchoTests::type_named;

// AST::access_effect_of is the sole answer to "what access does this parameter take", and it folds
// three spellings into one enum. these cases hold each of the three, and hold the conflict rule that
// reads them - the rule is what decides which programs AST::AccessPass may refuse, so it has to be
// asked here rather than only through whatever diagnostic happens to surface it

TEST_CASE("an access effect is read off the parameter it was written on", "[access]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function kernel(read int32& $src, inout int32& $dst, out int32& $slot) : void {}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "kernel");
    REQUIRE(decls.size() == 1);

    auto *kernel = decls[0];
    REQUIRE(kernel->args.size() == 3);

    REQUIRE(access_effect_of(*kernel, 0) == AccessEffect::t_read);
    REQUIRE(access_effect_of(*kernel, 1) == AccessEffect::t_inout);
    REQUIRE(access_effect_of(*kernel, 2) == AccessEffect::t_out);
}

TEST_CASE("a parameter with no effect written takes none", "[access]")
{
    // the migration boundary: an ordinary borrow is an address and says nothing, so nothing that
    // reads these answers may refuse a program that was legal before
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function swap(int32& $a, int32& $b) : void {}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "swap");
    REQUIRE(decls.size() == 1);

    REQUIRE(access_effect_of(*decls[0], 0) == AccessEffect::t_none);
    REQUIRE(access_effect_of(*decls[0], 1) == AccessEffect::t_none);
}

TEST_CASE("`mv` is the take effect under its own name", "[access]")
{
    // one answer and not two: `mv` had a keyword before the enum existed, and a parameter that
    // carries both flags must not read as two different accesses depending on who asks
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bag { int32 $v; }\n"
        "function consume(mv Bag $b) : void {}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "consume");
    REQUIRE(decls.size() == 1);

    REQUIRE(access_effect_of(*decls[0], 0) == AccessEffect::t_take);
    REQUIRE(decls[0]->signature_description() == "consume(mv Bag)");
}

TEST_CASE("a receiver's effect is what kind of member it belongs to", "[access]")
{
    // never written, and deliberately not stored: this is the same reader-not-a-store split
    // AST::receiver_is_const takes. it is also what makes the rule reach the standard library
    // without a keyword being added to a single one of its signatures
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bag {\n"
        "    int32 $v;\n"
        "    constructor() { $this->v = 0; }\n"
        "    destructor() {}\n"
        "    function set(int32 $x) : void { $this->v = $x; }\n"
        "    const function get() : int32 { return $this->v; }\n"
        "}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto *bag = type_named(m, "Bag");
    REQUIRE(bag != nullptr);

    // a constructor is the one member with no receiver *argument* - its `$this` is a body-local -
    // so the `out` fact sits on that declaration, which is also where the initialization half of the
    // rule has to read it
    REQUIRE(bag->constructors().size() == 1);

    auto *ctor = bag->constructors()[0];
    REQUIRE(ctor->body != nullptr);

    auto ctor_this = ctor->body->lookup_variable("$this");
    REQUIRE(ctor_this.decl != nullptr);
    REQUIRE(access_effect_of(*ctor_this.decl) == AccessEffect::t_out);

    auto *dtor = find_destructor(&bag->complex_type());
    REQUIRE(dtor != nullptr);
    REQUIRE(access_effect_of(*dtor, 0) == AccessEffect::t_take);

    auto setters = find_member_functions(&bag->complex_type(), "set");
    REQUIRE(setters.size() == 1);
    REQUIRE(access_effect_of(*setters[0], 0) == AccessEffect::t_inout);

    auto getters = find_member_functions(&bag->complex_type(), "get");
    REQUIRE(getters.size() == 1);
    REQUIRE(access_effect_of(*getters[0], 0) == AccessEffect::t_read);
}

TEST_CASE("an effect keyword before a `$name` is a type", "[access]")
{
    // the whole of what makes these contextual rather than lexed. a program that named a type `out`
    // before the effect existed still means what it meant
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct out { int32 $v; }\n"
        "function takes(out $o) : int32 { return $o->v; }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "takes");
    REQUIRE(decls.size() == 1);

    REQUIRE(access_effect_of(*decls[0], 0) == AccessEffect::t_none);
    REQUIRE(decls[0]->signature_description() == "takes(out)");
}

// AST::path_overlap is the region half. `t_unknown` is the answer that matters most: the refusal
// rule reads "is it t_overlap" and Phase 5's lowering will read "is it t_disjoint", so collapsing
// the middle answer into either one of them is wrong for the other

TEST_CASE("a const borrow is a read, and a plain borrow is nothing", "[access]")
{
    // the one inference, and the reason it is safe to make: a read is non-exclusive, so inferring
    // one can never on its own refuse anything. exclusivity is always written or a receiver
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bag { int32 $v; }\n"
        "function reads(const Bag& $a, Bag& $b) : void {}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "reads");
    REQUIRE(decls.size() == 1);

    REQUIRE(access_effect_of(*decls[0], 0) == AccessEffect::t_read);
    REQUIRE(access_effect_of(*decls[0], 1) == AccessEffect::t_none);

    // a by-value `const int32` copies and reaches none of the caller's storage, so it is not a read
    // of anything
    auto by_value = EchoTests::tests_make_parsed_bundle(
        "function counts(const int32 $n) : void {}\n");
    auto &m2 = by_value->modules.find_module("test");
    auto counts = decls_named(m2, "counts");
    REQUIRE(counts.size() == 1);
    REQUIRE(access_effect_of(*counts[0], 0) == AccessEffect::t_none);
}

TEST_CASE("two declarations that own their storage are disjoint", "[access]")
{
    AccessPath a;
    AccessPath b;

    // both null-rooted: nothing is known, and unknown is never disjoint
    REQUIRE(path_overlap(a, b) == Overlap::t_unknown);
}

TEST_CASE("a prefix contains what follows it", "[access]")
{
    // `$a` overlaps `$a->items[3]` - the case the whole rule exists for, and the one a naive
    // "are the paths equal" test would miss
    VarDeclNode *root = reinterpret_cast<VarDeclNode *>(0x1);

    AccessPath whole;
    whole.root = root;

    AccessPath deep;
    deep.root = root;
    deep.projections.push_back(Projection { ProjectionKind::t_field, "items", 0 });
    deep.projections.push_back(Projection { ProjectionKind::t_element, "", 3 });

    REQUIRE(path_overlap(whole, deep) == Overlap::t_overlap);
    REQUIRE(path_overlap(deep, whole) == Overlap::t_overlap);
    REQUIRE(path_overlap(deep, deep) == Overlap::t_overlap);
}

TEST_CASE("distinct fields and folded indices are disjoint, dynamic ones are not", "[access]")
{
    VarDeclNode *root = reinterpret_cast<VarDeclNode *>(0x1);

    AccessPath left;
    left.root = root;
    left.projections.push_back(Projection { ProjectionKind::t_field, "left", 0 });

    AccessPath right;
    right.root = root;
    right.projections.push_back(Projection { ProjectionKind::t_field, "right", 0 });

    REQUIRE(path_overlap(left, right) == Overlap::t_disjoint);

    AccessPath slot0;
    slot0.root = root;
    slot0.projections.push_back(Projection { ProjectionKind::t_element, "", 0 });

    AccessPath slot1;
    slot1.root = root;
    slot1.projections.push_back(Projection { ProjectionKind::t_element, "", 1 });

    REQUIRE(path_overlap(slot0, slot1) == Overlap::t_disjoint);
    REQUIRE(path_overlap(slot0, slot0) == Overlap::t_overlap);

    // two indices nothing folded are neither the same slot nor different ones - not even against
    // themselves, because two reads of `$i++` are two indices
    AccessPath dynamic;
    dynamic.root = root;
    dynamic.projections.push_back(Projection { ProjectionKind::t_dynamic, "", 0 });

    REQUIRE(path_overlap(dynamic, dynamic) == Overlap::t_unknown);
    REQUIRE(path_overlap(dynamic, slot0) == Overlap::t_unknown);

    // a field beside an index says nothing: the two paths stopped describing the same shape
    REQUIRE(path_overlap(left, slot0) == Overlap::t_unknown);
}

TEST_CASE("a root that only holds an address may name anything", "[access]")
{
    // **the exception that keeps this honest at a body boundary.** inside `extend`, `$this` and
    // `$other` are two roots and would otherwise read as disjoint - which is exactly the wrong
    // answer, and exactly why a call-site check cannot on its own license a `noalias`
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "struct Bag { int32 $v; }\n"
        "class Cls { int32 $v; }\n"
        "function f(Bag $owned, Bag& $borrowed, Cls $handle, ptr<Bag> $raw) : void {}\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "f");
    REQUIRE(decls.size() == 1);

    auto *f = decls[0];
    REQUIRE(f->args.size() == 4);

    REQUIRE(root_owns_its_storage(*f->args[0]));
    REQUIRE_FALSE(root_owns_its_storage(*f->args[1]));
    REQUIRE_FALSE(root_owns_its_storage(*f->args[2]));
    REQUIRE_FALSE(root_owns_its_storage(*f->args[3]));

    AccessPath owned;
    owned.root = f->args[0];

    AccessPath borrowed;
    borrowed.root = f->args[1];

    // two roots, and one of them is only an address - so this is unknown and never disjoint
    REQUIRE(path_overlap(owned, borrowed) == Overlap::t_unknown);
}

TEST_CASE("a T[N] index is an element projection, so two inout borrows of one slot conflict", "[access]")
{
    auto conflict = EchoTests::tests_make_parsed_bundle(
        "function poke(inout int32& $a, inout int32& $b) : void {}\n"
        "int32[4] $xs;\n"
        "poke(&$xs[0], &$xs[0]);\n");

    REQUIRE(EchoTests::has_issue_containing(*conflict, "same storage"));

    auto distinct = EchoTests::tests_make_parsed_bundle(
        "function poke(inout int32& $a, inout int32& $b) : void {}\n"
        "int32[4] $xs;\n"
        "poke(&$xs[0], &$xs[1]);\n");

    REQUIRE_FALSE(distinct->collector.has_critical_issues());
}

TEST_CASE("silence conflicts with nothing", "[access]")
{
    // exclusivity is not symmetric with silence. a parameter that declared no effect made no promise
    // anyone can be held to, so `t_none` beside an exclusive access is not a conflict - that is what
    // scopes the rule to code written for it
    REQUIRE_FALSE(access_effects_conflict(AccessEffect::t_none, AccessEffect::t_inout));
    REQUIRE_FALSE(access_effects_conflict(AccessEffect::t_none, AccessEffect::t_none));
    REQUIRE_FALSE(access_effects_conflict(AccessEffect::t_take, AccessEffect::t_none));

    // two reads may overlap - the one non-exclusive pairing
    REQUIRE_FALSE(access_effects_conflict(AccessEffect::t_read, AccessEffect::t_read));

    REQUIRE(access_effects_conflict(AccessEffect::t_read, AccessEffect::t_inout));
    REQUIRE(access_effects_conflict(AccessEffect::t_inout, AccessEffect::t_inout));
    REQUIRE(access_effects_conflict(AccessEffect::t_out, AccessEffect::t_read));
    REQUIRE(access_effects_conflict(AccessEffect::t_take, AccessEffect::t_read));
}

TEST_CASE("an effect survives being cloned into an instantiation", "[access]")
{
    // the monomorphizer clones a template body per instantiation, and VarDeclNode::clone is a
    // memberwise shallow copy plus its edges - so this holds for free and would break in silence,
    // leaving a generic kernel with no effects and its concrete twin with all of them
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "function kernel<T>(read T& $src, inout T& $dst) : void {}\n"
        "function main() : void { int32 $a = 1; int32 $b = 2; kernel<int32>(&$a, &$b); }\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto decls = decls_named(m, "kernel");

    // the template and its one instantiation
    REQUIRE(decls.size() == 2);

    for (auto *decl : decls) {
        REQUIRE(access_effect_of(*decl, 0) == AccessEffect::t_read);
        REQUIRE(access_effect_of(*decl, 1) == AccessEffect::t_inout);
    }
}

// ----------------------------------------------------------------------------------------------
// the safe/raw boundary
//
// a different question from everything above: those weigh two live accesses against each other, this
// one asks whether an access may be *trusted* at all. it is the half with no unit coverage until now,
// and the omission was not free - nine cases in four other suites went red the day the rule landed,
// each of them a `ptr<T>` fixture that had been legal since it was written

TEST_CASE("a borrow out of raw storage needs the word, and a block is what gives it", "[access]")
{
    const std::string raw =
        "$n = 1;\n"
        "ptr<int32> $p = &$n;\n";

    // `&$p:$[0]` offsets a raw address and borrows the result - the promotion, said the shortest way
    auto refused = EchoTests::tests_make_parsed_bundle(
        raw + "int32& $r = &$p:$[0];\necho $r;\n");

    REQUIRE(EchoTests::has_issue_containing(
        *refused, "cannot form 'int32&' from a raw address outside an 'unsafe' block"));

    // the same expression, discharged. nothing about the tree changes - the word is a permission, not
    // a lowering
    auto allowed = EchoTests::tests_make_parsed_bundle(
        raw + "unsafe { int32& $r = &$p:$[0]; echo $r; }\n");

    REQUIRE_FALSE(allowed->collector.has_critical_issues());

    // **and a body does not inherit it.** `_unsafe_depth` is saved and cleared across a function
    // boundary, so a declaration written inside a block carries no promise into code that shows no
    // sign of one. no e2e case can see this: both spellings are the same program otherwise
    auto nested = EchoTests::tests_make_parsed_bundle(
        "unsafe {\n"
        "    function first(ptr<int32> $q) : int32& { return &$q:$[0]; }\n"
        "}\n");

    REQUIRE(EchoTests::has_issue_containing(
        *nested, "cannot form 'int32&' from a raw address outside an 'unsafe' block"));
}

TEST_CASE("the address of a pointer local is not a promotion", "[access]")
{
    // **the place, never the operand's type.** `&$p` on a `ptr<T>` local addresses an ordinary slot -
    // the declaration *is* the storage and nothing raw was involved. reading the operand's type
    // instead made every `ptr<ptr<T>>` in the language a promotion, which is the shape of mistake a
    // rule stated over types rather than over places invites
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "$n = 1;\n"
        "ptr<int32> $p = &$n;\n"
        "ptr<ptr<int32>> $pp = &$p;\n");

    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    size_t seen = 0;

    // `&$n` and `&$p`, and neither reaches its storage through an address it read
    for (auto *addr : m.nodes.of_type<AddrOfExprNode>()) {
        REQUIRE_FALSE(place_is_raw_derived(addr->operand));
        REQUIRE_FALSE(borrow_promotes_raw_storage(addr->result_type(), addr->operand));
        seen++;
    }

    REQUIRE(seen == 2);
}
