#include <catch2/catch_test_macros.hpp>

#include <AST/ASTBundle.h>
#include <AST/ASTCoreTypes.h>
#include <AST/ASTUnwrap.h>
#include <AST/VarDeclNode.h>
#include <AST/ASTValueType.h>
#include <string>

#include "helpers.h"

// AST::unwrap_plan_for - **the sole answer to "how is this value unwrapped"**, and the unwrapping
// protocol's AST::iteration_plan_for.
//
// what these cases pin is the **arm order**, which is otherwise only a comment: the nullable arm is
// first, ahead of the unbound-core check that iteration puts first, and that inversion is what keeps a
// `T?` guarding with no standard library at all. `tests_make_parsed_bundle` links no stdlib, so every
// bundle here has an empty CoreTypes - which is exactly the condition the ordering is about.

namespace
{
    AST::ValueType type_of_local(AST::Bundle &bundle, const std::string &name)
    {
        for (auto *decl : bundle.modules.find_module("test").nodes.of_type<AST::VarDeclNode>()) {
            if (decl->name_full() == name && decl->has_type()) {
                return decl->type();
            }
        }

        return AST::ValueType::make_unknown();
    }
}

TEST_CASE("the nullable arm answers before the protocol is even looked for", "[AST][nullability]")
{
    // **the ordering, and why it is the opposite of iteration's.** `foreach` has no meaning without its
    // protocol, so a missing binding is a located refusal there. `guard` had a meaning before the
    // unwrapping protocol existed and keeps it - so a nullable is answered with an empty CoreTypes, which
    // is what `--no-stdlib` hands it and what contract.eco itself is compiled with
    auto bundle = EchoTests::tests_make_parsed_bundle(
        "int32? $maybe = 7;\n"
        "ptr<int32>? $addr = null;\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    AST::CoreTypes empty;
    REQUIRE_FALSE(empty.has(AST::CoreTypeKind::t_unwrappable));

    auto &registry = bundle->collector.type_registry;

    // the interned `{ __has, __value }` spelling
    const AST::UnwrapLookup tagged =
        AST::unwrap_plan_for(type_of_local(*bundle, "$maybe"), empty, registry);

    REQUIRE(tagged.result == AST::UnwrapLookup::Result::t_ok);
    REQUIRE(tagged.plan.kind == AST::UnwrapSource::t_builtin_nullable);
    REQUIRE(tagged.plan.payload_type.is_primitive_of_type(AST::ValueTypePrimitive::t_int32));
    REQUIRE_FALSE(tagged.plan.payload_type.is_nullable());

    // and the free-over-an-address spelling, which is the same answer
    const AST::UnwrapLookup flagged =
        AST::unwrap_plan_for(type_of_local(*bundle, "$addr"), empty, registry);

    REQUIRE(flagged.result == AST::UnwrapLookup::Result::t_ok);
    REQUIRE(flagged.plan.kind == AST::UnwrapSource::t_builtin_nullable);

    // **no failure type for either.** a nullable records that a value is absent and nothing about why,
    // which is what makes `else ($e)` over one a refusal with no arm anywhere
    REQUIRE_FALSE(tagged.plan.failure_type.has_value());
    REQUIRE_FALSE(flagged.plan.failure_type.has_value());
}

TEST_CASE("an unsettled subject is pending, never refused", "[AST][nullability]")
{
    // three results and not a bool, for AST::can_instantiate's reason: a subject the fixpoint has not
    // settled has to be distinguishable from one that cannot be unwrapped, or a guard reports
    // "'[unknown]' is always present" once per round and the first round's guess becomes the diagnostic
    auto bundle = EchoTests::tests_make_parsed_bundle("$x = 1;\n");

    AST::CoreTypes empty;
    auto &registry = bundle->collector.type_registry;

    const AST::UnwrapLookup unknown =
        AST::unwrap_plan_for(AST::ValueType::make_unknown(), empty, registry);

    REQUIRE(unknown.result == AST::UnwrapLookup::Result::t_pending);
    REQUIRE(unknown.refusal.empty());
}

TEST_CASE("a non-nullable subject with no protocol bound is refused, and says so", "[AST][nullability]")
{
    // the first arm that runs *after* the nullable one has declined, and the ordering is the point: with
    // nothing bound there is no interface to ask about, so the refusal says that rather than reporting a
    // conformance failure against a name this program has never heard of. it also says the part that
    // matters most - a `T?` still guards without the standard library
    auto bundle = EchoTests::tests_make_parsed_bundle("int32 $plain = 7;\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    AST::CoreTypes empty;

    const AST::UnwrapLookup lookup = AST::unwrap_plan_for(
        type_of_local(*bundle, "$plain"), empty, bundle->collector.type_registry);

    REQUIRE(lookup.result == AST::UnwrapLookup::Result::t_refused);
    REQUIRE(lookup.refusal.find("#[core: unwrappable]") != std::string::npos);
    REQUIRE(lookup.refusal.find("still guards without it") != std::string::npos);

    // and it is a refusal rather than a pending, which is what keeps a guard over a plainly wrong subject
    // from being re-reported once per round
    REQUIRE(lookup.plan.kind == AST::UnwrapSource::t_builtin_nullable);
}

TEST_CASE("guard_payload_refusal is silent on a widening and on an unsettled payload",
    "[AST][nullability]")
{
    // one wording for its two moments - the parser's, for a `T?`, and AST::GuardLowering's, for a subject
    // whose payload only a conformance could give. empty means "nothing to refuse", so an asker calls it
    // without asking a predicate first
    const AST::ValueType int32_t_(AST::ValueTypePrimitive::t_int32);
    const AST::ValueType bool_t_(AST::ValueTypePrimitive::t_bool);
    // **an address-like level**, because ValueType::make_nullable asserts on anything else: a tagged
    // optional is an interned layout and TypeRegistry::get_or_create_optional is the only thing that may
    // mint one. the subject only appears in the message here, so which nullable it is does not matter
    const AST::ValueType subject =
        AST::ValueType::make_nullable(AST::ValueType::make_pointer(int32_t_, false));

    // the same type
    REQUIRE(AST::guard_payload_refusal(int32_t_, int32_t_, subject).empty());

    // an unsettled payload is a question nobody has answered, not a mismatch - judging it would be a
    // round too early for AST::unwrap_plan_for's `t_pending` reason
    REQUIRE(AST::guard_payload_refusal(int32_t_, AST::ValueType::make_unknown(), subject).empty());

    // and a genuine mismatch is reported, naming all three types
    const std::string refusal = AST::guard_payload_refusal(bool_t_, int32_t_, subject);

    REQUIRE_FALSE(refusal.empty());
    REQUIRE(refusal.find("bool") != std::string::npos);
    REQUIRE(refusal.find("int32") != std::string::npos);
}
