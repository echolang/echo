#include <catch2/catch_test_macros.hpp>

#include <AST/ASTNamespace.h>
#include <AST/ASTTypeParam.h>
#include <AST/ASTValueType.h>

using namespace AST;

namespace {
    ValueType prim(ValueTypePrimitive p) { return ValueType(p); }

    // declares a type parameter on `owner`, the way the parser's declaring step does, so the
    // ordinal and the owner back-reference stay consistent
    TypeParamDecl *declare_param(TypeParamRegistry &params, ComplexType &owner, const std::string &name)
    {
        TypeParamDecl *decl = params.declare(name, owner.type_parameters.size());
        owner.add_type_parameter(decl);
        return decl;
    }
}

TEST_CASE("Default-constructed ValueType is well-defined unknown", "[types]")
{
    ValueType vt;
    REQUIRE(vt.get_kind() == ValueTypeKind::t_unknown);
    REQUIRE_FALSE(vt.is_primitive());
    REQUIRE_FALSE(vt.is_struct());
    // two default-constructed values compare equal (both unknown, no flags).
    REQUIRE(vt == ValueType());
}

TEST_CASE("substitute_type resolves a bare type parameter", "[types][generics]")
{
    TypeRegistry reg;
    TypeParamRegistry params;
    ComplexType box("Box");
    TypeParamDecl *t = declare_param(params, box, "T");

    TypeSubstitution subst = TypeSubstitution::positional(box.type_parameters, { prim(ValueTypePrimitive::t_int32) });

    ValueType resolved = substitute_type(ValueType::make_type_param(t), subst, reg);
    REQUIRE(resolved == prim(ValueTypePrimitive::t_int32));
}

TEST_CASE("substitute_type carries const/pointer flags onto the resolved type", "[types][generics]")
{
    TypeRegistry reg;
    TypeParamRegistry params;
    ComplexType box("Box");
    TypeParamDecl *t = declare_param(params, box, "T");

    TypeSubstitution subst = TypeSubstitution::positional(box.type_parameters, { prim(ValueTypePrimitive::t_int32) });

    ValueType param = ValueType::make_pointer(ValueType::make_type_param(t));
    param.set_const(true);

    ValueType resolved = substitute_type(param, subst, reg);
    REQUIRE(resolved.is_pointer());
    REQUIRE(resolved.is_const());
    REQUIRE(resolved.is_primitive_of_type(ValueTypePrimitive::t_int32));
}

TEST_CASE("substitute_type leaves primitives and concrete types unchanged", "[types][generics]")
{
    TypeRegistry reg;
    TypeSubstitution subst;

    REQUIRE(substitute_type(prim(ValueTypePrimitive::t_bool), subst, reg)
            == prim(ValueTypePrimitive::t_bool));

    ComplexType point("Point");           // non-generic struct
    point.add_property("x", prim(ValueTypePrimitive::t_float32));
    ValueType point_type = ValueType::make_struct(&point);
    REQUIRE(substitute_type(point_type, subst, reg) == point_type);
}

TEST_CASE("TypeRegistry interns instantiations by (template, args) identity", "[types][generics]")
{
    TypeRegistry reg;

    TypeParamRegistry params;
    ComplexType box("Box");               // struct Box<T> { T value; }
    TypeParamDecl *t = declare_param(params, box, "T");
    box.add_property("value", ValueType::make_type_param(t));

    ComplexType* box_i1 = reg.get_or_create_instantiation(&box, { prim(ValueTypePrimitive::t_int32) });
    ComplexType* box_i2 = reg.get_or_create_instantiation(&box, { prim(ValueTypePrimitive::t_int32) });
    ComplexType* box_f  = reg.get_or_create_instantiation(&box, { prim(ValueTypePrimitive::t_float32) });

    // same args => same pointer (this is what ValueType struct equality relies on).
    REQUIRE(box_i1 == box_i2);
    // different args => distinct instantiation.
    REQUIRE(box_i1 != box_f);

    // the property was substituted to the concrete type.
    REQUIRE(box_i1->property_count() == 1);
    REQUIRE(box_i1->get_property_type(0) == prim(ValueTypePrimitive::t_int32));
    REQUIRE(box_f->get_property_type(0) == prim(ValueTypePrimitive::t_float32));

    // the instance records its origin.
    REQUIRE(box_i1->is_instantiated());
    REQUIRE(box_i1->template_ref == &box);
    REQUIRE(box_i1->name.value() == "Box<int32>");
}

TEST_CASE("substitute_type resolves nested generic applications", "[types][generics]")
{
    TypeRegistry reg;

    // struct Bar<U> { U u; }
    TypeParamRegistry params;
    ComplexType bar("Bar");
    TypeParamDecl *u = declare_param(params, bar, "U");
    bar.add_property("u", ValueType::make_type_param(u));

    // struct Foo<T> { Bar<T> inner; }
    ComplexType foo("Foo");
    TypeParamDecl *t = declare_param(params, foo, "T");

    // the application Bar<T>, i.e. Bar instantiated with Foo's own parameter
    ComplexType* bar_of_T = reg.get_or_create_instantiation(&bar, { ValueType::make_type_param(t) });
    foo.add_property("inner", ValueType::make_struct(bar_of_T));

    // instantiate Foo<int32>: inner must become Bar<int32>, whose own property is int32.
    ComplexType* foo_int = reg.get_or_create_instantiation(&foo, { prim(ValueTypePrimitive::t_int32) });
    REQUIRE(foo_int->property_count() == 1);

    ValueType inner = foo_int->get_property_type(0);
    REQUIRE(inner.is_struct());

    ComplexType* inner_ct = inner.get_complex_type();
    REQUIRE(inner_ct->is_instantiated());
    REQUIRE(inner_ct->template_ref == &bar);
    REQUIRE(inner_ct->get_property_type(0) == prim(ValueTypePrimitive::t_int32));

    // the Bar<int32> reached through Foo<int32> is the same interned type as one built directly.
    ComplexType* bar_int_direct = reg.get_or_create_instantiation(&bar, { prim(ValueTypePrimitive::t_int32) });
    REQUIRE(inner_ct == bar_int_direct);
}

TEST_CASE("Mangled names distinguish same-named types from different namespaces", "[types]")
{
    NamespaceManager namespaces;

    ComplexType foo_a("Foo");
    foo_a.ast_namespace = &namespaces.retrieve("a");

    ComplexType foo_b("Foo");
    foo_b.ast_namespace = &namespaces.retrieve("b");

    ValueType type_a = ValueType::make_struct(&foo_a);
    ValueType type_b = ValueType::make_struct(&foo_b);

    REQUIRE(type_a.get_mangled_name() != type_b.get_mangled_name());

    // the mangled name is stable, nothing about it is derived from the type's address
    REQUIRE(type_a.get_mangled_name() == ValueType::make_struct(&foo_a).get_mangled_name());

    // the namespace also shows up in diagnostics
    REQUIRE(type_a.get_type_desciption() == "a::Foo");
    REQUIRE(type_b.get_type_desciption() == "b::Foo");
}

TEST_CASE("Mangled names are unambiguous across namespace depths", "[types]")
{
    NamespaceManager namespaces;

    // a::b::Foo, a::Foo and a root struct literally named "a_b_Foo" must all stay distinct,
    // which is what the length prefixes in the mangled token buy us
    ComplexType nested("Foo");
    nested.ast_namespace = &namespaces.retrieve("a::b");

    ComplexType shallow("Foo");
    shallow.ast_namespace = &namespaces.retrieve("a");

    ComplexType flat("a_b_Foo");
    flat.ast_namespace = &namespaces.root();

    std::string nested_mangled = ValueType::make_struct(&nested).get_mangled_name();
    std::string shallow_mangled = ValueType::make_struct(&shallow).get_mangled_name();
    std::string flat_mangled = ValueType::make_struct(&flat).get_mangled_name();

    REQUIRE(nested_mangled != shallow_mangled);
    REQUIRE(nested_mangled != flat_mangled);
    REQUIRE(shallow_mangled != flat_mangled);

    REQUIRE(ValueType::make_struct(&nested).get_type_desciption() == "a::b::Foo");

    // a type in the root namespace mangles like an unqualified one, no namespace wrapper
    ComplexType rootless("Foo");
    REQUIRE(flat_mangled == "MLC7a_b_Foo");
    REQUIRE(ValueType::make_struct(&rootless).get_mangled_name() == "MLC3Foo");
    REQUIRE(ValueType::make_struct(&rootless).get_type_desciption() == "Foo");
}

TEST_CASE("Generic instantiations mangle their arguments recursively", "[types][generics]")
{
    NamespaceManager namespaces;
    TypeRegistry reg;

    ComplexType foo_a("Foo");
    foo_a.ast_namespace = &namespaces.retrieve("a");

    ComplexType foo_b("Foo");
    foo_b.ast_namespace = &namespaces.retrieve("b");

    ComplexType box("Box");
    box.ast_namespace = &namespaces.retrieve("c");
    TypeParamRegistry params;
    TypeParamDecl *t = declare_param(params, box, "T");
    box.add_property("item", ValueType::make_type_param(t));

    ComplexType *box_of_a = reg.get_or_create_instantiation(&box, { ValueType::make_struct(&foo_a) });
    ComplexType *box_of_b = reg.get_or_create_instantiation(&box, { ValueType::make_struct(&foo_b) });

    std::string mangled_a = ValueType::make_struct(box_of_a).get_mangled_name();
    std::string mangled_b = ValueType::make_struct(box_of_b).get_mangled_name();

    // Box<a::Foo> and Box<b::Foo> are different types - the monomorphizer keys its instance
    // cache on these strings, so a collision here would hand out the wrong instance
    REQUIRE(mangled_a != mangled_b);

    // an instantiation inherits its template's namespace
    REQUIRE(box_of_a->ast_namespace == box.ast_namespace);
    REQUIRE(ValueType::make_struct(box_of_a).get_type_desciption() == "c::Box<a::Foo>");

    // the display name is no longer what gets mangled, so no symbol-hostile characters leak in
    for (auto illegal : { '<', '>', ',', ' ', '*', ':' }) {
        REQUIRE(mangled_a.find(illegal) == std::string::npos);
    }
}
