#include <catch2/catch_test_macros.hpp>

#include <AST/ASTValueType.h>

using namespace AST;

namespace {
    ValueType prim(ValueTypePrimitive p) { return ValueType(p); }
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
    TypeSubstitution subst{ prim(ValueTypePrimitive::t_int32) };

    ValueType resolved = substitute_type(ValueType::make_type_param(0), subst, reg);
    REQUIRE(resolved == prim(ValueTypePrimitive::t_int32));
}

TEST_CASE("substitute_type carries const/pointer flags onto the resolved type", "[types][generics]")
{
    TypeRegistry reg;
    TypeSubstitution subst{ prim(ValueTypePrimitive::t_int32) };

    ValueType param = ValueType::make_pointer(ValueType::make_type_param(0));
    param.set_const(true);

    ValueType resolved = substitute_type(param, subst, reg);
    REQUIRE(resolved.is_pointer());
    REQUIRE(resolved.is_const());
    REQUIRE(resolved.is_primitive_of_type(ValueTypePrimitive::t_int32));
}

TEST_CASE("substitute_type leaves primitives and concrete types unchanged", "[types][generics]")
{
    TypeRegistry reg;
    TypeSubstitution subst{ prim(ValueTypePrimitive::t_float64) };

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

    ComplexType box("Box");               // struct Box<T> { T value; }
    box.type_parameters.push_back({"T"});
    box.add_property("value", ValueType::make_type_param(0));

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
    ComplexType bar("Bar");
    bar.type_parameters.push_back({"U"});
    bar.add_property("u", ValueType::make_type_param(0));

    // the application Bar<T> (T = the enclosing param, index 0).
    ComplexType* bar_of_T = reg.get_or_create_instantiation(&bar, { ValueType::make_type_param(0) });

    // struct Foo<T> { Bar<T> inner; }
    ComplexType foo("Foo");
    foo.type_parameters.push_back({"T"});
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
