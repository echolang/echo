#include <catch2/catch_test_macros.hpp>

#include <AST/ASTTypeParam.h>
#include <AST/ASTValueType.h>
#include <AST/TypeDeclNode.h>
#include <unordered_set>

#include "helpers.h"

using namespace AST;

namespace
{
    TypeParamDecl *declare_param(TypeParamRegistry &params, ComplexType &owner, const std::string &name)
    {
        TypeParamDecl *decl = params.declare(name, owner.type_parameters.size());
        owner.add_type_parameter(decl);
        return decl;
    }

    // the interned applications of `tmpl`, including the template's own self-application
    size_t applications_of(const Collector &collector, const ComplexType *tmpl)
    {
        size_t count = 0;
        for (const auto *ct : collector.type_registry.instantiations()) {
            if (ct->template_ref == tmpl) {
                count++;
            }
        }
        return count;
    }
}

TEST_CASE("A declared type parameter carries its name, ordinal and owner", "[types][generics]")
{
    TypeParamRegistry params;
    ComplexType box("Box");

    TypeParamDecl *t = declare_param(params, box, "T");

    REQUIRE(t->name == "T");
    REQUIRE(t->ordinal == 0);
    REQUIRE(t->owner_kind() == TypeParamOwnerKind::t_type);
    REQUIRE(t->owner_type() == &box);
    REQUIRE(t->owner_name() == "Box");

    // qualified for diagnostics, so two same-named parameters can be told apart in a message
    REQUIRE(t->describe() == "Box::T");

    // a second parameter gets the next ordinal, and add_type_parameter asserts they agree
    TypeParamDecl *u = declare_param(params, box, "U");
    REQUIRE(u->ordinal == 1);
    REQUIRE(box.type_parameters.size() == 2);
}

TEST_CASE("An unconstrained parameter allows anything, a constrained one only its set", "[types][generics]")
{
    TypeParamRegistry params;
    ComplexType box("Box");
    TypeParamDecl *t = declare_param(params, box, "T");

    REQUIRE_FALSE(t->is_constrained());
    REQUIRE(t->allows(ValueType(ValueTypePrimitive::t_bool)));

    t->constraint.push_back(ValueType(ValueTypePrimitive::t_int32));
    t->constraint_spelling = "int";

    REQUIRE(t->is_constrained());
    REQUIRE(t->allows(ValueType(ValueTypePrimitive::t_int32)));
    REQUIRE_FALSE(t->allows(ValueType(ValueTypePrimitive::t_bool)));

    // const is ignored when matching, so `const int` still satisfies `int`
    REQUIRE(t->allows(ValueType::make_const(ValueType(ValueTypePrimitive::t_int32))));

    // pointerness is not: `T: int` must reject ptr<int> and int&. the decay that lets a
    // pointer argument bind a bare T is a call-boundary rule in Monomorphizer::unify
    REQUIRE_FALSE(t->allows(ValueType::make_pointer(ValueType(ValueTypePrimitive::t_int32), true)));
    REQUIRE_FALSE(t->allows(ValueType::make_pointer(ValueType(ValueTypePrimitive::t_int32), false)));
}

TEST_CASE("Type parameters of different owners are distinct types", "[types][generics]")
{
    TypeParamRegistry params;

    ComplexType box("Box");                       // struct Box<T>
    ComplexType pair("Pair");                     // struct Pair<A, B>

    TypeParamDecl *box_t = declare_param(params, box, "T");
    TypeParamDecl *pair_a = declare_param(params, pair, "A");

    // both are their owner's first parameter, and under an ordinal-only representation they
    // would compare equal and hash alike
    REQUIRE(box_t->ordinal == pair_a->ordinal);

    ValueType t = ValueType::make_type_param(box_t);
    ValueType a = ValueType::make_type_param(pair_a);

    REQUIRE_FALSE(t == a);
    REQUIRE(t == ValueType::make_type_param(box_t));

    std::unordered_set<ValueType> distinct { t, a };
    REQUIRE(distinct.size() == 2);

    // each owner only recognises its own
    REQUIRE(box.declares_type_param(t));
    REQUIRE_FALSE(box.declares_type_param(a));
    REQUIRE(pair.declares_type_param(a));
    REQUIRE_FALSE(pair.declares_type_param(t));
}

TEST_CASE("substitute_type leaves an unbound type parameter untouched", "[types][generics]")
{
    TypeRegistry reg;
    TypeParamRegistry params;

    ComplexType box("Box");
    TypeParamDecl *t = declare_param(params, box, "T");

    // an empty substitution covers nothing, so T survives. under the previous positional
    // representation this tripped an assert on the substitution's size
    TypeSubstitution empty;
    ValueType unresolved = substitute_type(ValueType::make_type_param(t), empty, reg);
    REQUIRE(unresolved.is_type_param());
    REQUIRE(unresolved.get_type_param() == t);
}

TEST_CASE("A partial substitution resolves only the parameters it binds", "[types][generics]")
{
    TypeRegistry reg;
    TypeParamRegistry params;

    // the shape of a generic member of a generic owner: the owner declares T, the member U.
    // instantiating the owner must resolve T and leave U generic for the member's own turn
    ComplexType owner("Box");
    TypeParamDecl *t = declare_param(params, owner, "T");

    ComplexType member("push");
    TypeParamDecl *u = declare_param(params, member, "U");

    TypeSubstitution subst;
    subst.bind(t, ValueType(ValueTypePrimitive::t_int32));

    REQUIRE(subst.covers(t));
    REQUIRE_FALSE(subst.covers(u));

    REQUIRE(substitute_type(ValueType::make_type_param(t), subst, reg)
            == ValueType(ValueTypePrimitive::t_int32));

    ValueType still_generic = substitute_type(ValueType::make_type_param(u), subst, reg);
    REQUIRE(still_generic.is_type_param());
    REQUIRE(still_generic.get_type_param() == u);

    // and inside a generic application: Pair<T, U> becomes Pair<int32, U>
    ComplexType pair("Pair");
    declare_param(params, pair, "X");
    declare_param(params, pair, "Y");

    ComplexType *pair_of_t_u = reg.get_or_create_instantiation(
        &pair, { ValueType::make_type_param(t), ValueType::make_type_param(u) });

    ValueType substituted = substitute_type(ValueType::make_struct(pair_of_t_u), subst, reg);
    ComplexType *result = substituted.get_complex_type();

    REQUIRE(result->instantiation_args.size() == 2);
    REQUIRE(result->instantiation_args[0] == ValueType(ValueTypePrimitive::t_int32));
    REQUIRE(result->instantiation_args[1].is_type_param());
    REQUIRE(result->instantiation_args[1].get_type_param() == u);
}

TEST_CASE("TypeRegistry keeps same-ordinal parameters of different templates apart", "[types][generics]")
{
    TypeRegistry reg;
    TypeParamRegistry params;

    ComplexType box("Box");
    ComplexType pair("Pair");
    ComplexType holder("Holder");

    TypeParamDecl *box_t = declare_param(params, box, "T");
    TypeParamDecl *pair_a = declare_param(params, pair, "A");
    declare_param(params, holder, "H");

    // Holder<Box::T> and Holder<Pair::A> are different applications, though both arguments are
    // the first parameter of their owner
    ComplexType *of_box_t = reg.get_or_create_instantiation(&holder, { ValueType::make_type_param(box_t) });
    ComplexType *of_pair_a = reg.get_or_create_instantiation(&holder, { ValueType::make_type_param(pair_a) });

    REQUIRE(of_box_t != of_pair_a);

    // and the same argument still interns to the same application
    REQUIRE(of_box_t == reg.get_or_create_instantiation(&holder, { ValueType::make_type_param(box_t) }));
}

TEST_CASE("A type parameter renders with the name the user wrote", "[types][generics]")
{
    TypeParamRegistry params;
    ComplexType box("Box");
    TypeParamDecl *t = declare_param(params, box, "T");

    ValueType plain = ValueType::make_type_param(t);
    REQUIRE(plain.get_type_desciption() == "T");

    ValueType decorated = ValueType::make_const(ValueType::make_pointer(plain, true));
    REQUIRE(decorated.get_type_desciption() == "const ptr<T>");

    // const binds to the level it sits on, so these two are different types
    ValueType const_pointee = ValueType::make_pointer(ValueType::make_const(plain), true);
    REQUIRE(const_pointee.get_type_desciption() == "ptr<const T>");
    REQUIRE(const_pointee != decorated);

    // a borrow spells itself with a trailing &
    REQUIRE(ValueType::make_pointer(plain, false).get_type_desciption() == "T&");

    // the mangled form stays the ordinal: it reaches the LLVM symbol table, which has to be
    // reproducible across runs rather than derived from the declaration's address
    REQUIRE(plain.get_mangled_name() == "MLT0");
}

TEST_CASE("Type parameter scopes nest, inner shadowing outer", "[parser][generics]")
{
    auto env = EchoTests::tests_make_parser_env("");
    auto &context = env.payload.context;

    TypeParamRegistry params;
    ComplexType owner("Box");
    ComplexType member("push");

    TypeParamDecl *outer_t = declare_param(params, owner, "T");
    TypeParamDecl *inner_u = declare_param(params, member, "U");
    TypeParamDecl *inner_t = params.declare("T", 1);

    // nothing is in scope to begin with
    REQUIRE(context.find_type_param("T") == nullptr);

    context.push_type_param_scope({ outer_t });
    REQUIRE(context.find_type_param("T") == outer_t);
    REQUIRE(context.find_type_param("U") == nullptr);

    // an inner scope adds to the outer one instead of replacing it — this is what lets a
    // generic member of a generic struct name both its own and its owner's parameters
    context.push_type_param_scope({ inner_u });
    REQUIRE(context.find_type_param("U") == inner_u);
    REQUIRE(context.find_type_param("T") == outer_t);

    context.pop_type_param_scope();
    REQUIRE(context.find_type_param("U") == nullptr);
    REQUIRE(context.find_type_param("T") == outer_t);

    // a same-named inner parameter shadows the outer one, and popping restores it
    context.push_type_param_scope({ inner_t });
    REQUIRE(context.find_type_param("T") == inner_t);
    context.pop_type_param_scope();
    REQUIRE(context.find_type_param("T") == outer_t);

    context.pop_type_param_scope();
    REQUIRE(context.find_type_param("T") == nullptr);
}

TEST_CASE("An empty inner scope leaves the enclosing parameters visible", "[parser][generics]")
{
    auto env = EchoTests::tests_make_parser_env("");
    auto &context = env.payload.context;

    TypeParamRegistry params;
    ComplexType owner("Box");
    TypeParamDecl *t = declare_param(params, owner, "T");

    context.push_type_param_scope({ t });

    // a non-generic member pushes an empty frame; the owner's T must survive both the push
    // and the pop. the previous flat list cleared on entry and on exit, wiping T twice
    {
        AST::TypeParamScope member_scope(context, {});
        REQUIRE(context.find_type_param("T") == t);
    }

    REQUIRE(context.find_type_param("T") == t);
    context.pop_type_param_scope();
}

TEST_CASE("Re-parsing a generic struct reuses its type parameter declarations", "[parser][generics]")
{
    // a module is parsed twice — a symbol pass then a full pass — each with a fresh Context
    // if the second pass minted new declarations, the struct's self-application Foo<T> would
    // intern a second time and the two would compare unequal
    auto bundle = EchoTests::tests_make_parsed_bundle("struct Box<T> { T $value; }\n");
    REQUIRE_FALSE(bundle->collector.has_critical_issues());

    auto &m = bundle->modules.find_module("test");
    auto structs = m.nodes.of_type<TypeDeclNode>();
    REQUIRE(structs.size() == 1);

    auto *box = structs[0];
    REQUIRE(box->is_generic());
    REQUIRE(box->type_parameters().size() == 1);

    const TypeParamDecl *t = box->type_parameters()[0];
    REQUIRE(t->name == "T");
    REQUIRE(t->owner_type() == &box->complex_type());

    // exactly one interned application of the template: its own Box<T>. a second declaration
    // set would show up here as a second entry
    REQUIRE(applications_of(bundle->collector, &box->complex_type()) == 1);
}

TEST_CASE("A duplicate type parameter name is reported", "[parser][generics]")
{
    EchoTests::assert_code_emits_issue(
        "function id<T, T>(T $x): T { return $x; }\n",
        "Type parameter 'T' is already declared in this list");
}
