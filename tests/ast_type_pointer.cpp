#include <catch2/catch_test_macros.hpp>

#include <AST/ASTNamespace.h>
#include <AST/ASTTypeParam.h>
#include <AST/ASTValueType.h>

#include <unordered_set>

using namespace AST;

namespace
{
    ValueType prim(ValueTypePrimitive p) { return ValueType(p); }

    ValueType ptr_to(ValueType pointee) { return ValueType::make_pointer(pointee, true); }
    ValueType ref_to(ValueType pointee) { return ValueType::make_pointer(pointee, false); }
}

TEST_CASE( "A pointer nests instead of saturating", "[types][pointer]" )
{
    ValueType i32 = prim(ValueTypePrimitive::t_int32);
    ValueType once = ptr_to(i32);
    ValueType twice = ptr_to(once);

    REQUIRE(once.is_pointer());
    REQUIRE(twice.is_pointer());

    // the whole point of the recursive type: these are three distinct types, where the old
    // idempotent pointer bit made the last two the same
    REQUIRE(once != i32);
    REQUIRE(twice != once);

    REQUIRE(once.pointee() == i32);
    REQUIRE(twice.pointee() == once);
    REQUIRE(twice.pointee().pointee() == i32);
}

TEST_CASE( "A pointer is not its pointee's kind", "[types][pointer]" )
{
    ValueType p = ptr_to(prim(ValueTypePrimitive::t_int32));

    // `ptr<int32>` used to answer true to all of these, because it was an int32 carrying a
    // flag. every predicate that reasons about the pointee must now say so explicitly
    REQUIRE_FALSE(p.is_primitive());
    REQUIRE_FALSE(p.is_integer_type());
    REQUIRE_FALSE(p.is_numeric_type());
    REQUIRE(value_type_of(p).is_integer_type());
}

TEST_CASE( "value_type_of reaches exactly one level", "[types][pointer]" )
{
    ValueType i32 = prim(ValueTypePrimitive::t_int32);

    REQUIRE(value_type_of(i32) == i32);
    REQUIRE(value_type_of(ptr_to(i32)) == i32);

    // one level, never more - `ptr<ptr<uint8>>` reads as `ptr<uint8>`
    REQUIRE(value_type_of(ptr_to(ptr_to(i32))) == ptr_to(i32));
}

TEST_CASE( "ptr<T> and T& differ only in nullability", "[types][pointer]" )
{
    ValueType i32 = prim(ValueTypePrimitive::t_int32);
    ValueType nullable = ptr_to(i32);
    ValueType borrow = ref_to(i32);

    REQUIRE(nullable.is_nullable());
    REQUIRE_FALSE(borrow.is_nullable());
    REQUIRE(nullable.pointee() == borrow.pointee());

    // but they are still distinct types, so a registry keyed on equality keeps them apart
    REQUIRE(nullable != borrow);
}

TEST_CASE( "const binds to the level it sits on", "[types][pointer]" )
{
    ValueType i32 = prim(ValueTypePrimitive::t_int32);

    ValueType const_pointer = ValueType::make_const(ptr_to(i32));  // const ptr<int32>
    ValueType const_pointee = ptr_to(ValueType::make_const(i32));  // ptr<const int32>

    // the single const bit of the old flag model could not tell these apart
    REQUIRE(const_pointer != const_pointee);

    REQUIRE(const_pointer.is_const());
    REQUIRE_FALSE(const_pointer.pointee().is_const());

    REQUIRE_FALSE(const_pointee.is_const());
    REQUIRE(const_pointee.pointee().is_const());
}

TEST_CASE( "Pointer types render their spelling", "[types][pointer]" )
{
    ValueType i32 = prim(ValueTypePrimitive::t_int32);

    REQUIRE(ptr_to(i32).get_type_desciption() == "ptr<int32>");
    REQUIRE(ref_to(i32).get_type_desciption() == "int32&");
    REQUIRE(ptr_to(ptr_to(prim(ValueTypePrimitive::t_uint8))).get_type_desciption() == "ptr<ptr<uint8>>");

    REQUIRE(ValueType::make_const(ptr_to(i32)).get_type_desciption() == "const ptr<int32>");
    REQUIRE(ptr_to(ValueType::make_const(i32)).get_type_desciption() == "ptr<const int32>");

    // a borrow spells its pointee's const outward, the doc's read-only borrow
    REQUIRE(ref_to(ValueType::make_const(i32)).get_type_desciption() == "const int32&");
}

TEST_CASE( "Pointer mangling is recursive and leaves pointer-free types untouched", "[types][pointer]" )
{
    ValueType i32 = prim(ValueTypePrimitive::t_int32);

    // the historical encoding for a non-pointer is unchanged, so only signatures that actually
    // take a pointer get a new LLVM symbol
    REQUIRE(i32.get_mangled_name() == "MLPi");

    REQUIRE(ptr_to(i32).get_mangled_name() == "MRNMLPi");
    REQUIRE(ref_to(i32).get_mangled_name() == "MRBMLPi");
    REQUIRE(ptr_to(ptr_to(i32)).get_mangled_name() == "MRNMRNMLPi");

    // the two const positions mangle apart, matching the type identity
    REQUIRE(ValueType::make_const(ptr_to(i32)).get_mangled_name() == "CRNMLPi");
    REQUIRE(ptr_to(ValueType::make_const(i32)).get_mangled_name() == "MRNCLPi");
}

TEST_CASE( "Equal pointer types hash equal, distinct ones do not collide", "[types][pointer]" )
{
    ValueType i32 = prim(ValueTypePrimitive::t_int32);
    std::hash<ValueType> h;

    REQUIRE(h(ptr_to(i32)) == h(ptr_to(i32)));

    // a bare xor of the pointee hash would make these two equal, which would in turn make
    // Box<int32> and Box<ptr<int32>> collide in the TypeRegistry's intern map
    REQUIRE(h(ptr_to(i32)) != h(i32));

    std::unordered_set<ValueType> seen;
    seen.insert(i32);
    seen.insert(ptr_to(i32));
    seen.insert(ref_to(i32));
    seen.insert(ptr_to(ptr_to(i32)));
    REQUIRE(seen.size() == 4);
}

TEST_CASE( "A generic instantiation interns per pointer depth", "[types][pointer][generics]" )
{
    TypeRegistry reg;
    TypeParamRegistry params;

    ComplexType box("Box");
    TypeParamDecl *t = params.declare("T", 0);
    box.add_type_parameter(t);
    box.add_property("item", ValueType::make_type_param(t));

    ValueType i32 = prim(ValueTypePrimitive::t_int32);

    ComplexType *of_value = reg.get_or_create_instantiation(&box, { i32 });
    ComplexType *of_pointer = reg.get_or_create_instantiation(&box, { ptr_to(i32) });
    ComplexType *of_borrow = reg.get_or_create_instantiation(&box, { ref_to(i32) });

    // three different layouts, so three different interned types
    REQUIRE(of_value != of_pointer);
    REQUIRE(of_pointer != of_borrow);

    // and the same argument still interns back to the same instance
    REQUIRE(of_pointer == reg.get_or_create_instantiation(&box, { ptr_to(i32) }));

    REQUIRE(of_pointer->get_property_type("item") == ptr_to(i32));
}

TEST_CASE( "contains_type_param sees through a pointer", "[types][pointer][generics]" )
{
    TypeParamRegistry params;
    ComplexType box("Box");
    TypeParamDecl *t = params.declare("T", 0);
    box.add_type_parameter(t);

    ValueType open = ptr_to(ptr_to(ValueType::make_type_param(t)));
    REQUIRE(contains_type_param(open));

    REQUIRE_FALSE(contains_type_param(ptr_to(prim(ValueTypePrimitive::t_int32))));
}

TEST_CASE( "target_type_of follows every level, value_type_of exactly one", "[types][pointer]" )
{
    ValueType i32 = prim(ValueTypePrimitive::t_int32);

    // two rules that must not be conflated. reading is one auto-deref, so `ptr<ptr<int32>>`
    // reads as `ptr<int32>`. `->` instead reaches the struct however deep it is, which is what
    // makes `$pp->x` mean the same as `$p->x`
    REQUIRE(value_type_of(ptr_to(ptr_to(i32))) == ptr_to(i32));
    REQUIRE(target_type_of(ptr_to(ptr_to(i32))) == i32);

    REQUIRE(target_type_of(i32) == i32);
    REQUIRE(target_type_of(ref_to(ptr_to(ref_to(i32)))) == i32);
}

TEST_CASE( "Implicit convertibility is looser than identity, and directional", "[types][pointer]" )
{
    ValueType i32 = prim(ValueTypePrimitive::t_int32);

    // top level const is dropped, so a const argument satisfies a mutable parameter
    REQUIRE(is_implicitly_convertible(ValueType::make_const(i32), i32));

    // a borrow widens to a nullable pointer - it only discards the non-null guarantee
    REQUIRE(is_implicitly_convertible(ref_to(i32), ptr_to(i32)));

    // the reverse asserts non-nullness and needs the explicit cast
    REQUIRE_FALSE(is_implicitly_convertible(ptr_to(i32), ref_to(i32)));

    // a pointer does NOT convert to its pointee here. the auto-deref that makes one usable
    // where its pointee is expected is a read, and the adjustment pass writes it into the tree
    // as an explicit deref - so a value-position pointer read already has the pointee's type
    // by the time anything asks. allowing it would also accept `$p = &$b`, storing an address
    // into the pointee's slot
    REQUIRE_FALSE(is_implicitly_convertible(ptr_to(i32), i32));

    // and taking an address stays visible in the source
    REQUIRE_FALSE(is_implicitly_convertible(i32, ptr_to(i32)));
    REQUIRE_FALSE(is_implicitly_convertible(i32, ref_to(i32)));
}

TEST_CASE( "A borrow may gain const on its pointee, never lose it", "[types][pointer]" )
{
    ValueType i32 = prim(ValueTypePrimitive::t_int32);
    ValueType const_i32 = ValueType::make_const(i32);

    // `const int32& $r` is the doc's read-only borrow and the recommended parameter form
    // (book/concept/pointers_and_refs_v2.md, "Const"). every `int32&` satisfies it, because
    // promising only to read is weaker than being allowed to write
    REQUIRE(is_implicitly_convertible(ref_to(i32), ref_to(const_i32)));
    REQUIRE(is_implicitly_convertible(ref_to(i32), ptr_to(const_i32)));
    REQUIRE(is_implicitly_convertible(ptr_to(i32), ptr_to(const_i32)));

    // the other direction would launder the promise away, so it stays an error
    REQUIRE_FALSE(is_implicitly_convertible(ref_to(const_i32), ref_to(i32)));
    REQUIRE_FALSE(is_implicitly_convertible(ptr_to(const_i32), ptr_to(i32)));

    // and the nullability rule still applies on top of it, in both directions
    REQUIRE_FALSE(is_implicitly_convertible(ptr_to(i32), ref_to(const_i32)));

    // const on the pointer itself is a different question from const on the pointee, so it
    // does not open the same door
    REQUIRE_FALSE(is_implicitly_convertible(ValueType::make_const(ref_to(const_i32)), ref_to(i32)));
}

TEST_CASE( "substitute_type keeps const at the level it was written", "[types][pointer][generics]" )
{
    TypeRegistry reg;
    TypeParamRegistry params;
    TypeParamDecl *t = params.declare("T", 0);
    ValueType i32 = prim(ValueTypePrimitive::t_int32);

    TypeSubstitution subst;
    subst.bind(t, i32);

    // `ptr<const T>` must come back as `ptr<const int32>`, not `const ptr<int32>` - the two are
    // distinct types, and collapsing them was exactly what the old const bit-flag did
    ValueType const_pointee = ptr_to(ValueType::make_const(ValueType::make_type_param(t)));
    ValueType resolved_pointee = substitute_type(const_pointee, subst, reg);

    REQUIRE(resolved_pointee == ptr_to(ValueType::make_const(i32)));
    REQUIRE_FALSE(resolved_pointee.is_const());
    REQUIRE(resolved_pointee.pointee().is_const());

    // and the mirror case, const on the pointer itself
    ValueType const_pointer = ValueType::make_const(ptr_to(ValueType::make_type_param(t)));
    ValueType resolved_pointer = substitute_type(const_pointer, subst, reg);

    REQUIRE(resolved_pointer == ValueType::make_const(ptr_to(i32)));
    REQUIRE(resolved_pointer.is_const());
    REQUIRE_FALSE(resolved_pointer.pointee().is_const());
}
