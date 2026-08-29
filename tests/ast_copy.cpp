#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTCopy.h>
#include <AST/ASTMemberLookup.h>
#include <AST/AssignNode.h>
#include <AST/ExprNode.h>
#include <AST/FunctionDeclNode.h>
#include <AST/ScopeNode.h>
#include <AST/TypeDeclNode.h>
#include <AST/VarDeclNode.h>

#include "helpers.h"

using namespace AST;

using EchoTests::has_issue_containing;
using EchoTests::type_named;

// --- the classifier ------------------------------------------------------------------------------

namespace
{
    // the types every case below is asked about, declared and nothing more. **no copy site anywhere**:
    // a copy of a synthesizable type would make the pass write the constructor, and the type would then
    // classify as t_constructor - which is true of it afterwards and not what these cases are asking
    const char *k_types =
        // the reference kinds
        "class Handle { int32 $tag; }\n"
        "class Written {\n"
        "    int32 $tag;\n"
        "    constructor(int32 $tag) { $this->tag = $tag; }\n"
        "    constructor(Written& $other) { $this->tag = $other->tag; }\n"
        "}\n"
        "interface Shape { function area() : int32; }\n"

        // nothing to arrange
        "struct Plain { int32 $x; usize $y; }\n"
        "struct HoldsPointer { ptr<uint8> $data; }\n"

        // the author's answer, with and without a destructor of its own. a written copy constructor
        // is a constructor, so it deletes memberwise - the value constructors below are what the
        // copy-site fixture still uses to build `$a`
        "struct Says {\n"
        "    int32 $x;\n"
        "    constructor(int32 $x) { $this->x = $x; }\n"
        "    constructor(Says& $other) { $this->x = $other->x; }\n"
        "}\n"
        "struct SaysAndOwns {\n"
        "    usize $tag;\n"
        "    ptr<uint8> $data;\n"
        "    constructor(usize $tag, ptr<uint8> $data) { $this->tag = $tag; $this->data:$ = $data; }\n"
        "    constructor(SaysAndOwns& $other) { $this->tag = $other->tag; $this->data:$ = $other->data; }\n"
        "    destructor() { $this->data:$ = null; }\n"
        "}\n"

        // the compiler's own, directly and transitively
        "struct HoldsClass { Handle $h; int32 $n; }\n"
        "struct HoldsHoldsClass { HoldsClass $inner; }\n"
        "struct HoldsSays { Says $s; }\n"

        // and the refusals
        "struct OwnsRaw { ptr<uint8> $data; destructor() { $this->data:$ = null; } }\n"
        "struct HoldsClassAndOwns { Handle $h; destructor() { } }\n"
        "struct HoldsOwnsRaw { OwnsRaw $inner; }\n"

        // one template, two instantiations, and neither answer is the template's
        "struct Box<T> { T $item; }\n";

    CopyKind kind_of(Module &m, const char *type_name)
    {
        TypeDeclNode *decl = type_named(m, type_name);

        if (decl == nullptr) {
            FAIL("no type named " << type_name);
        }

        return classify_copy(decl->value_type());
    }
}

TEST_CASE("classify_copy answers the reference kinds with a retain", "[copy]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_types);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    REQUIRE(kind_of(m, "Handle") == CopyKind::t_retain);

    // an interface value is a class handle wearing an erased type, and what is inside is a question the
    // type cannot answer - so it is beside the class rather than below it, where the property walk would
    // find no properties and call it uncopyable
    REQUIRE(kind_of(m, "Shape") == CopyKind::t_retain);

    // **the load-bearing order**: a class that declares a `Written&` constructor is still copied by
    // retaining it. that constructor builds a *new* object, which is a different operation from what
    // `$b = $a` means
    REQUIRE(find_copy_constructor(&type_named(m, "Written")->complex_type()) != nullptr);
    REQUIRE(kind_of(m, "Written") == CopyKind::t_retain);

    // the two reference kinds with no declaration to read: a callable shares its captured environment,
    // and one more weak reference is one more reference
    REQUIRE(classify_copy(ValueType::make_callable(ValueType::make_void(), {})) == CopyKind::t_retain);
    REQUIRE(classify_copy(ValueType::make_weak(type_named(m, "Handle")->value_type())) == CopyKind::t_retain);
}

TEST_CASE("classify_copy answers a value that owns nothing with a byte copy", "[copy]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_types);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    REQUIRE(classify_copy(EchoTests::prim(ValueTypePrimitive::t_int32)) == CopyKind::t_bytes);

    // a borrow is copied by copying the address, which is what a borrow is - so a borrow of an
    // *uncopyable* type is copyable
    REQUIRE(classify_copy(ValueType::make_pointer(type_named(m, "OwnsRaw")->value_type(), false))
        == CopyKind::t_bytes);

    REQUIRE(kind_of(m, "Plain") == CopyKind::t_bytes);

    // ownership ends at a raw pointer: a struct holding one and saying nothing about teardown owns
    // nothing as far as the type system can tell
    REQUIRE(kind_of(m, "HoldsPointer") == CopyKind::t_bytes);
}

TEST_CASE("classify_copy prefers the constructor its author wrote", "[copy]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_types);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // and deliberately not gated on ownership: `Says` owns nothing, and `$b = $a` still means what
    // `Says($a)` means, or which operation you got would depend on whether it happens to declare a
    // destructor as well
    REQUIRE(kind_of(m, "Says") == CopyKind::t_constructor);
    REQUIRE(kind_of(m, "SaysAndOwns") == CopyKind::t_constructor);
}

TEST_CASE("classify_copy folds a struct's answer from its properties", "[copy]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_types);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // a retain per class field and nothing to guess
    REQUIRE(kind_of(m, "HoldsClass") == CopyKind::t_synthesizable);

    // transitively, through a struct-typed property that is itself synthesizable
    REQUIRE(kind_of(m, "HoldsHoldsClass") == CopyKind::t_synthesizable);

    // **a property that says how it is copied counts too**, even though it owns nothing: the fold asks
    // what copying each property *does*, not whether it owns something. `HoldsSays` would otherwise be
    // byte-copied and `Says`' own constructor silently skipped - see
    // tests_eco/structs/copy_field_with_copy_constructor.eco
    REQUIRE(kind_of(m, "HoldsSays") == CopyKind::t_synthesizable);
}

TEST_CASE("a tagged optional folds its answer from its payload", "[copy]")
{
    // **`T?` is a layout with two properties**, so it needs no arm of its own here: the same fold that
    // answers for a struct answers for it, from `__has` (a bool, always bytes) and `__value`.
    //
    // that is the whole reason the pair is a type rather than a per-level flag. while it was a flag, this
    // function read straight through it and answered about the payload - so an `array<int32>?` claimed to
    // copy the way an `array<int32>` does, over a value that was a tagged pair
    auto bundle = EchoTests::tests_make_parsed_bundle(k_types);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto &registry = bundle->collector.type_registry;

    const auto optional_of = [&](const ValueType &payload) {
        return classify_copy(registry.get_or_create_optional(payload));
    };

    // a payload that owns nothing keeps the byte copy, which is the no-regression story: `int32?` and
    // `Point?` reach not one new line of the ownership pass
    REQUIRE(optional_of(EchoTests::prim(ValueTypePrimitive::t_int32)) == CopyKind::t_bytes);
    REQUIRE(optional_of(EchoTests::type_named(m, "Plain")->value_type()) == CopyKind::t_bytes);

    // a payload with a rule needs the pair to be built rather than copied, because the payload's own
    // copy has to be *called* - and only when the tag says there is one
    REQUIRE(optional_of(EchoTests::type_named(m, "Says")->value_type()) == CopyKind::t_synthesizable);
    REQUIRE(optional_of(EchoTests::type_named(m, "HoldsClass")->value_type()) == CopyKind::t_synthesizable);

    // and a payload nobody gave a rule refuses, at the pair. this is what makes `Owns? $b = $a` a located
    // error at the author's own line rather than a byte copy of an owner
    REQUIRE(optional_of(EchoTests::type_named(m, "OwnsRaw")->value_type()) == CopyKind::t_none);

    // a class handle is the *other* spelling of `T?` - the flag, where a null address already means absent -
    // so it never reaches the fold at all and stays one retain
    REQUIRE(classify_copy(
        ValueType::make_nullable(EchoTests::type_named(m, "Handle")->value_type())) == CopyKind::t_retain);
}

TEST_CASE("classify_copy refuses what nobody has given a rule", "[copy]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_types);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // the split worth stating: not "we cannot copy owners", but "we cannot copy owners we have no rule
    // for". ownership ends at a raw pointer, and the type holding it is the only thing that knows what
    // duplicating it would mean
    REQUIRE(kind_of(m, "OwnsRaw") == CopyKind::t_none);

    // a destructor is the author saying this teardown is theirs, and it refuses even a struct whose
    // properties would all have answered - which is what separates this ladder from needs_destruction
    REQUIRE(kind_of(m, "HoldsClassAndOwns") == CopyKind::t_none);

    // one property with no rule is enough, however deep: it would be copied as bytes alongside the ones
    // that do have one, leaving two owners of one resource
    REQUIRE(kind_of(m, "HoldsOwnsRaw") == CopyKind::t_none);
}

TEST_CASE("classify_copy answers per instantiation, not per template", "[copy]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_types);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto &types = bundle->collector.type_registry;

    ComplexType &tmpl = type_named(m, "Box")->complex_type();

    ComplexType *of_int = types.get_or_create_instantiation(&tmpl, {EchoTests::prim(ValueTypePrimitive::t_int32)});
    ComplexType *of_handle = types.get_or_create_instantiation(&tmpl, {type_named(m, "Handle")->value_type()});
    ComplexType *of_raw = types.get_or_create_instantiation(&tmpl, {type_named(m, "OwnsRaw")->value_type()});

    REQUIRE(classify_copy(ValueType::make_struct(of_int)) == CopyKind::t_bytes);
    REQUIRE(classify_copy(ValueType::make_struct(of_handle)) == CopyKind::t_synthesizable);
    REQUIRE(classify_copy(ValueType::make_struct(of_raw)) == CopyKind::t_none);

    // and the bare template answers t_bytes rather than refusing: `T` is a not-yet, and a round of the
    // fixpoint that has not settled it must not report anything
    REQUIRE(kind_of(m, "Box") == CopyKind::t_bytes);
}

TEST_CASE("the two predicates are spellings over the classifier", "[copy]")
{
    auto bundle = EchoTests::tests_make_parsed_bundle(k_types);
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");

    // whatever is added to CopyKind, these two cannot come apart from it - which is what stops a caller
    // of either from re-deriving the ladder
    for (const char *name : {"Handle", "Plain", "Says", "HoldsClass", "OwnsRaw"}) {
        const ValueType type = type_named(m, name)->value_type();

        REQUIRE(copy_needs_constructor(type) == (classify_copy(type) != CopyKind::t_bytes));
        REQUIRE(copy_is_synthesizable(type) == (classify_copy(type) == CopyKind::t_synthesizable));
    }
}

// --- the pairing ---------------------------------------------------------------------------------
//
// **the classification and the branch the ownership pass takes are one decision.** the divergence A24
// was filed for would be silent and silent in the double-free direction, so it is pinned directly here:
// the same type is classified, and then copied, and the two answers must agree

namespace
{
    // the value `$b` is initialized with, after the pass has rewritten it
    ExprNode *copied_value(ScopeNode &body)
    {
        for (auto &child : body.children) {
            if (child.has_type<VarDeclNode>() && child.get<VarDeclNode>().name_full() == "$b") {
                return child.get<VarDeclNode>().init_expr;
            }
        }

        FAIL("no declaration of $b in the body");
        throw std::runtime_error("unreachable");
    }

    // **the arm the pass took, read back off the tree.** the observable difference between t_constructor
    // and t_synthesizable is not in the copy - by design, nothing downstream can tell them apart - but
    // in whether the type had a constructor before the copy site asked for one, which is what
    // `declared_its_own` carries in from the classifier's own fixture
    CopyKind branch_taken(Bundle &bundle, Module &m, bool declared_its_own)
    {
        if (has_issue_containing(bundle, "cannot be copied")) {
            return CopyKind::t_none;
        }

        REQUIRE_FALSE(bundle.collector.has_critical_issues());

        ExprNode *value = copied_value(*EchoTests::decls_named(m, "f").at(0)->body);
        REQUIRE(value != nullptr);

        if (value->get_node_type() == NodeType::n_expr_retain) {
            return CopyKind::t_retain;
        }

        if (value->get_node_type() == NodeType::n_expr_call) {
            auto *call = static_cast<FunctionCallExprNode *>(value);

            // a copy and not some other call: the declaration is the type's own copy constructor, which
            // is the same bargain a drop makes
            REQUIRE(call->decl != nullptr);
            REQUIRE(call->decl == find_copy_constructor(call->decl->get_return_type().get_complex_type()));

            return declared_its_own ? CopyKind::t_constructor : CopyKind::t_synthesizable;
        }

        return CopyKind::t_bytes;
    }

    struct Row
    {
        const char *type_name;
        const char *construct;
        CopyKind expected;
    };
}

TEST_CASE("every CopyKind is the branch the ownership pass takes for it", "[copy]")
{
    const std::vector<Row> rows = {
        {"Plain", "Plain(1, 2)", CopyKind::t_bytes},
        {"HoldsPointer", "HoldsPointer(null)", CopyKind::t_bytes},
        {"Handle", "Handle(1)", CopyKind::t_retain},
        {"Written", "Written(1)", CopyKind::t_retain},
        {"Says", "Says(1)", CopyKind::t_constructor},
        {"SaysAndOwns", "SaysAndOwns(1, null)", CopyKind::t_constructor},
        {"HoldsClass", "HoldsClass(Handle(1), 2)", CopyKind::t_synthesizable},
        {"HoldsHoldsClass", "HoldsHoldsClass(HoldsClass(Handle(1), 2))", CopyKind::t_synthesizable},
        {"HoldsSays", "HoldsSays(Says(1))", CopyKind::t_synthesizable},
        {"OwnsRaw", "OwnsRaw(null)", CopyKind::t_none},
        {"HoldsClassAndOwns", "HoldsClassAndOwns(Handle(1))", CopyKind::t_none},
        {"HoldsOwnsRaw", "HoldsOwnsRaw(OwnsRaw(null))", CopyKind::t_none},
    };

    for (const Row &row : rows) {
        // the classification, asked of a program with no copy in it at all
        auto declared = EchoTests::tests_make_parsed_bundle(k_types);
        REQUIRE_FALSE(declared->collector.has_critical_issues());

        auto &declared_module = declared->modules.find_module("test");
        const CopyKind kind = kind_of(declared_module, row.type_name);

        INFO("classifying " << row.type_name);
        REQUIRE(kind == row.expected);

        const bool declared_its_own =
            find_copy_constructor(&type_named(declared_module, row.type_name)->complex_type()) != nullptr;

        // ...and the branch, asked of the same types with one copy added
        auto copied = EchoTests::tests_make_parsed_bundle(
            std::string(k_types) + "function f() : void {\n"
            "    " + row.type_name + " $a = " + row.construct + ";\n"
            "    " + row.type_name + " $b = $a;\n"
            "}\n");

        auto &copied_module = copied->modules.find_module("test");

        INFO("copying " << row.type_name);
        REQUIRE(branch_taken(*copied, copied_module, declared_its_own) == kind);
    }
}
