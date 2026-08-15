#ifndef ASTVALUETYPE_H
#define ASTVALUETYPE_H

#pragma once

#include "AST/ASTVisibility.h"

#include <type_traits>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <unordered_map>
#include <tuple>
#include <optional>
#include <unordered_set>
#include <cstdint>
#include <cassert>
#include <concepts>
#include <iostream>

namespace AST
{
    class ComplexType;
    class TypeRegistry;
    class Namespace;

    // everything this header does with a type parameter works on an incomplete type (store,
    // compare and hash a pointer, hold a vector of them), which is what keeps ASTValueType.h
    // free of any dependency on ASTTypeParam.h
    class TypeParamDecl;

    // a member function of a ComplexType, for the same reason: this header only ever stores and
    // hands back the pointers. matching one by name needs the complete node, so that rule lives
    // in ASTMemberLookup.h instead
    class FunctionDeclNode;

    // a type nested inside a ComplexType, stored the same way and for the same reason: this header
    // only ever stores and hands back the pointer
    class TypeDeclNode;
    class VarDeclNode;

    enum class ValueTypeKind
    {
        t_primitive,
        t_class,
        t_struct,
        // a declared capability: `interface Drawable`. a named type like the two above and reached
        // through the same ComplexType, so member lookup, mangling and rendering are shared - what
        // separates it is that it has no properties of its own and never a layout. as a *value* it
        // exists only over a class, because dispatch needs the runtime identity only a class carries
        t_interface,
        // a closed set of alternatives: `enum DistanceUnit`. a named type reached through the same
        // ComplexType as the three above, with a layout like a struct's - `__tag` first, then one
        // property per payload field - so member access, TBAA, debug info and the whole copy and
        // teardown taxonomy apply to it unchanged.
        //
        // **a kind rather than a flag on the layout, which is the opposite call from is_optional** and
        // for t_weak's reason. an optional is a flag precisely so the is_struct() sites go on answering
        // yes, because everything they do to a struct is right for one. an enum is the reverse: its
        // slots are the compiler's, not the author's, so a site that admits it as a struct would let
        // `$e->__tag = 3` through and would synthesize a field-wise constructor seating a discriminant
        // by hand. as a kind each one refuses until an arm is added on purpose
        t_enum,
        t_generic,
        t_pointer,
        // a non-owning handle on a counted object: `weak<Foo>`. recursive like a pointer, holding the
        // class it names one level down, and at runtime it is literally the same address - what makes it
        // its own type is which count it moves and that reading it needs an upgrade
        //
        // **a kind rather than a flag on t_class, and that is the whole safety argument.** as a flag,
        // every one of the many `is_class()` sites - member lookup, `->`, interface widening, argument
        // fit, the copy taxonomy - would go on answering yes and silently read through a handle whose
        // object may be gone. as a kind, `is_class()` is false and each of those refuses it until an arm
        // is added on purpose. it is the same reason `t_pointer` is a kind and not a flag
        t_weak,
        // a callable value: `function<R(P...)>`. structural like a pointer and for the same reason -
        // it carries nothing but its signature, so two spellings of one signature must be one type
        t_callable,
        t_unknown
    };

    // **the two properties of a tagged optional, by index.** `ComplexType::is_optional` says a layout is
    // one; these say what is in it. one declaration for both ends, because the AST reaches them by name
    // through find_property and codegen reaches them by index through a GEP, and the two agreeing is what
    // makes a load land on the field it meant.
    //
    // `__has` is **first, and false is zero**, so an all-zero value is an absent one - which is what lets
    // an absent optional be a `zeroinitializer` and nothing else.
    //
    // `unsigned` rather than `size_t` so that an index reaches an `llvm::ArrayRef<unsigned>` - a GEP and an
    // `insertvalue` both take one, and a narrowing there would need a cast at every codegen site or a
    // second pair of constants beside these, which is the drift this declaration exists to prevent
    constexpr unsigned k_optional_has_index = 0;
    constexpr unsigned k_optional_value_index = 1;
    constexpr const char *k_optional_has_name = "__has";
    constexpr const char *k_optional_value_name = "__value";

    // **the discriminant of an enum, by index** - the same declaration the pair above is, for its
    // reason, and first for the reason `__has` is first: a case is identified by its ordinal, ordinal
    // zero is the first case written, so an all-zero enum value is that first case and nothing else.
    //
    // a payload field is property `k_enum_tag_index + 1` and onward, named by
    // AST::enum_payload_property_name - the tag is the only slot with a fixed index, because how many
    // properties precede a given case's payload is what ComplexType::EnumCase records
    constexpr unsigned k_enum_tag_index = 0;
    constexpr const char *k_enum_tag_name = "__tag";

    // `__c<ordinal>_<field>` - the property a payload field of a case occupies. one function rather
    // than a format string at each of the four sites that build or read one (the layout synthesis, the
    // case constructor, the pattern binding and the teardown), since a name that is derived in four
    // places is a name that can differ in one
    std::string enum_payload_property_name(size_t case_ordinal, const std::string &field_name);

    // which of the four storage classes a named type was declared as. the distinction is one tag and
    // nearly everything else about them is identical - one parser, one declaration node, one member
    // store - so it is a tag rather than separate types. carried on ComplexType, since that is the
    // only thing a generic instantiation has
    //
    // t_interface is the odd one and deliberately sits here rather than in a node of its own: an
    // interface is declared, named, namespaced, generic and mangled exactly as the other two are, and
    // its requirements live in the same `_methods` list a struct's methods do. what it does *not* have
    // is properties or a layout, which is a refusal at its declaration rather than a different shape
    //
    // t_enum is the fourth for that same argument read forwards: it is declared, named, namespaced,
    // generic and mangled identically, its methods and statics live in the same two lists, and what it
    // adds is a case table beside them plus a set of refusals at its declaration. what it does *not*
    // add is a second declaration parser or a second answer to any member-lookup question
    enum class ComplexTypeKind
    {
        t_struct,
        t_class,
        t_interface,
        t_enum,
    };

    // **the keyword this kind was declared with** - "struct", "class", "interface", "enum".
    //
    // one owner rather than the `is_class() ? ... : is_interface_kind() ? ...` ladder each reader
    // would otherwise write, because those readers are diagnostics and a dump, and a kind added
    // without an arm in one of them reads as a *different kind* rather than as a missing case. the
    // switch has no tail for that reason
    const char *type_kind_keyword(ComplexTypeKind kind);

    // flags apply to the level they sit on, which is what makes `ptr<const T>` (const pointee)
    // and `const ptr<T>` (const pointer) distinct types. t_nullable only ever sits on a
    // t_pointer level: set for `ptr<T>`, clear for the non-nullable borrow `T&`
    enum class ValueTypeFlags
    {
        t_const = 1 << 0,
        t_nullable = 1 << 1,
    };

    enum class ValueTypePrimitive
    {
        t_complex,
        t_int8,
        t_int16,
        t_int32,
        t_int64,
        t_uint8,
        t_uint16,
        t_uint32,
        t_uint64,
        // pointer-width integers, the width coming from ECO_TARGET_POINTER_SIZE. deliberately
        // their own primitives rather than aliases of t_uint64/t_int64: every count, length and
        // capacity in the stdlib is spelled with these, and a distinct type identity is what
        // keeps those signatures stable when the width changes
        t_usize,
        t_isize,
        t_float32,
        t_float64,
        t_bool,
        t_void,
    };

    struct IntegerSize
    {
        uint8_t size;
        bool is_signed;

        IntegerSize(uint8_t size, bool is_signed) : size(size), is_signed(is_signed) {}

        // how wide this type is, in bits. the one place that turns the byte count into a bit
        // count - both bounds below rest on it, and so does the shift-count refusal in
        // AST::const_fold, which is a fact about the same width and not a second question
        unsigned bit_width() const { return static_cast<unsigned>(size) * 8; }

        // the bounds are built by shifting a full mask *down* rather than shifting 1 *up* by the
        // width. `1ULL << 64` is undefined behaviour, and because `size` arrives from a
        // non-inlined switch it was a runtime shift: arm64 and x86 mask the count to 6 bits, so
        // the widest unsigned type reported a maximum of 0 and rejected every literal assigned
        // to a uint64. shifting down never reaches a width-sized shift count
        int64_t get_max_negative_value() const {
            if (!is_signed) return 0;
            const unsigned bits = bit_width();
            // -2^(bits-1), negated in unsigned space because negating the int64 minimum is
            // itself signed overflow. unsigned wrap-around is defined, and so is the conversion
            const uint64_t magnitude = 1ULL << (bits - 1);
            return static_cast<int64_t>(~magnitude + 1);
        }

        uint64_t get_max_positive_value() const {
            const unsigned bits = bit_width();
            const unsigned value_bits = is_signed ? bits - 1 : bits;
            return ~0ULL >> (64 - value_bits);
        }
    };

    constexpr bool is_struct(ValueTypeKind kind) {
        return kind == ValueTypeKind::t_struct;
    }

    constexpr bool is_class(ValueTypeKind kind) {
        return kind == ValueTypeKind::t_class;
    }

    constexpr bool is_interface(ValueTypeKind kind) {
        return kind == ValueTypeKind::t_interface;
    }

    constexpr bool is_enum(ValueTypeKind kind) {
        return kind == ValueTypeKind::t_enum;
    }

    std::string get_primitive_name(ValueTypePrimitive primitive);
    uint8_t get_primitive_size(ValueTypePrimitive primitive);
    char get_primitive_id_char(ValueTypePrimitive primitive);
    IntegerSize get_integer_size(ValueTypePrimitive primitive);

    // what a `function<R(P...)>` is: a return type and a parameter list, and nothing else. defined
    // below ValueType, which it is made of; only the handle appears inside the class
    struct CallableSignature;

    class ValueType
    {
        friend struct std::hash<ValueType>;

    public:

        static ValueType make_void() {
            return ValueType(ValueTypePrimitive::t_void);
        }

        static ValueType make_unknown() {
            return ValueType(ValueTypeKind::t_unknown, ValueTypePrimitive::t_void);
        }

        static ValueType void_type() {
            return ValueType(ValueTypePrimitive::t_void);
        }

        // the type of a named declaration, struct or class, taking the kind from the ComplexType
        // itself. this is the one that should be called: it cannot produce a t_class ValueType over a
        // struct's layout or the reverse, which the two spellings below can
        static ValueType make_complex(ComplexType *complex_type, const std::vector<ValueType> &args = {}, TypeRegistry *registry = nullptr);

        // both assert the ComplexType agrees. kept because a caller that genuinely knows which kind it
        // is asking for reads better saying so, and because the assert catches the mismatch at the
        // construction site rather than wherever the wrong type later fails to lower
        static ValueType make_struct(ComplexType *complex_type, const std::vector<ValueType> &args = {}, TypeRegistry *registry = nullptr);

        static ValueType make_class(ComplexType *complex_type, const std::vector<ValueType> &args = {}, TypeRegistry *registry = nullptr);

        static ValueType make_type_param(const TypeParamDecl *param) {
            assert(param);
            return ValueType(ValueTypeKind::t_generic, param);
        }

        // const applies to the level it is attached to, so make_const(make_pointer(t)) is
        // `const ptr<T>` while make_pointer(make_const(t)) is `ptr<const T>`
        static ValueType make_const(ValueType type) {
            type.type_flags |= static_cast<uint8_t>(ValueTypeFlags::t_const);
            return type;
        }

        static ValueType make_mutable(ValueType type) {
            type.type_flags &= ~static_cast<uint8_t>(ValueTypeFlags::t_const);
            return type;
        }

        // `T?` **where the flag is the whole implementation** - a pointer, a class handle, a weak, whose
        // null address already means absent. shaped like the two above and composing the same way:
        // `make_nullable(make_pointer(t, false))` is `ptr<T>` and is *the same type* as the `T&?` a
        // program could now write, because it is the same two bits.
        //
        // **it asserts, and the assert is the design.** over anything else `T?` is a tagged pair, and a
        // pair is a *type* that has to be interned - AST::TypeRegistry::get_or_create_optional. A pure
        // static cannot reach a registry, so the two cases cannot share one entry point, and the four
        // sites in the compiler that build a nullable type all either hold a registry or are reaching for
        // a class. A silent flag on a struct here is what the whole feature was broken by
        //
        // **idempotent** for the same reason it always was: `T??` is `T?`, there being one `null` in the
        // language and nothing for a second level of absence to mean. deliberately the opposite choice
        // from a pointer, where a second level is a genuinely different type
        static ValueType make_nullable(ValueType type) {
            // **and a type parameter, which is the one case that is neither.** `T?` inside a template is
            // nullable of *something*, and which of the two spellings it becomes cannot be known until `T`
            // is bound - a layout over an unsubstituted parameter would be a pair whose payload is a name.
            // So the flag carries it, is_wrapped_optional() says no because there is no layout, and
            // AST::substitute_type asks this same question again at the instantiation, where it has an answer
            assert((type.has_null_representation() || type.is_type_param())
                && "a nullable non-address level is a type - use TypeRegistry::get_or_create_optional");
            type.type_flags |= static_cast<uint8_t>(ValueTypeFlags::t_nullable);
            return type;
        }

        // **unwrapping needs no registry**, which is what keeps this pure and total: a tagged optional
        // already holds its payload as a property, so `T?` -> `T` is a read rather than a construction.
        // the asymmetry with make_nullable above is real and is the reason the change is as small as it is
        static ValueType make_non_nullable(ValueType type) {
            if (type.is_wrapped_optional()) {
                return type.optional_payload();
            }

            type.type_flags &= ~static_cast<uint8_t>(ValueTypeFlags::t_nullable);
            return type;
        }

        // `nullable` picks the spelling: true is `ptr<T>`, false the non-nullable borrow `T&`
        // both are one machine address; nullability is the only thing the type system checks.
        // deliberately NOT idempotent - make_pointer(make_pointer(int32)) is ptr<ptr<int32>>
        static ValueType make_pointer(ValueType pointee, bool nullable) {
            ValueType type(ValueTypeKind::t_pointer, ValueTypePrimitive::t_void);
            type._pointee = std::make_shared<const ValueType>(std::move(pointee));
            if (nullable) {
                type.type_flags |= static_cast<uint8_t>(ValueTypeFlags::t_nullable);
            }
            return type;
        }

        // `weak<Foo>`. the target is held in the same shared_ptr slot a pointee is, for the same reason:
        // this is a recursive kind, and nothing about it needs interning to compare
        //
        // asserts its target is a class or a type parameter - a weak reference over anything else is
        // meaningless, since nothing else is counted. the *diagnostic* for a written `weak<int32>` is the
        // parser's, at the type, so by the time this is reached the question is already settled
        static ValueType make_weak(ValueType class_type) {
            assert((class_type.is_class() || class_type.is_type_param())
                && "a weak reference is only meaningful over a counted type");
            ValueType type(ValueTypeKind::t_weak, ValueTypePrimitive::t_void);
            type._pointee = std::make_shared<const ValueType>(std::move(class_type));
            return type;
        }

        // `function<R(P...)>`. the signature is shared rather than interned, so equality is structural
        // all the way down - see CallableSignature
        static ValueType make_callable(ValueType return_type, std::vector<ValueType> parameter_types);

        ValueType() = default;
        ValueType(ValueTypePrimitive primitive) :
            kind(ValueTypeKind::t_primitive),
            primitive(primitive)
        {}

        inline ComplexType *get_complex_type() const {
            assert(has_complex_type());
            return _complex_type;
        }

        bool is_type_param() const {
            return kind == ValueTypeKind::t_generic;
        }

        const TypeParamDecl *get_type_param() const {
            assert(is_type_param());
            return _type_param;
        }

        ValueTypeKind get_kind() const {
            return kind;
        }

        uint8_t get_type_flags() const {
            return type_flags;
        }

        bool is_const() const {
            return type_flags & static_cast<uint8_t>(ValueTypeFlags::t_const);
        }

        bool is_pointer() const {
            return kind == ValueTypeKind::t_pointer;
        }

        bool is_callable() const {
            return kind == ValueTypeKind::t_callable;
        }

        bool is_weak() const {
            return kind == ValueTypeKind::t_weak;
        }

        // **is this level one machine address, whose null value is already "absent"?**
        //
        // the one question that decides what `T?` costs. a pointer, a class handle and a weak handle all
        // lower to an opaque `ptr`, so a null one *is* the empty case and `T?` over them is free - no
        // extra word, no wrapping, the same value flowing through unchanged. everything else - a
        // primitive, a struct, an interface's two words, a callable's two words - has no spare
        // representation to donate, so `T?` over it lowers to `{ i1 __has, T }`
        //
        // it lives here rather than in TypeLowering because three subsystems have to agree on it: the
        // lowering that picks the shape, the coercion that wraps and unwraps, and the comparison that
        // tests one against `null`. two of them answering differently is a wrong load with no diagnostic
        bool has_null_representation() const {
            return is_pointer() || is_class() || is_weak();
        }

        // `T?` over something with no spare null value, so it is a tagged pair rather than the payload
        // itself.
        //
        // **it is a layout, and this is the one place that says so.** the pair is a real interned type
        // with two properties (AST::TypeRegistry::get_or_create_optional), which is what lets every
        // ownership question about it be answered by the machinery that answers them for any other struct.
        // The two implementations of `T?` are still exactly the split has_null_representation() describes:
        // a flag where the flag *is* the implementation, a type where it cannot be
        bool is_wrapped_optional() const;

        // the payload of a wrapped optional - `T` for a `T?`. asserts, so a reader who has not asked
        // is_wrapped_optional() first cannot be handed a struct's first property by accident
        const ValueType &optional_payload() const;

        // the class a weak reference names. a separate accessor from pointee() rather than a widening of
        // it, so that a reader who meant "one level down a pointer" cannot silently be handed a weak's
        // target - the two are one machine address and nothing downstream would notice
        const ValueType &weak_target() const {
            assert(is_weak() && _pointee);
            return *_pointee;
        }

        const CallableSignature &signature() const;

        // **may this level be absent?** true for `ptr<T>` and for any `T?`, false for the borrow `T&` and
        // for a bare `T`. one flag on the level it sits on, exactly like const
        //
        // it used to assert `is_pointer()`, because a pointer level was the only place nullability could
        // sit and `ptr<T>` versus `T&` was the whole of it. generalising the flag is what gives the
        // language `T?` over anything, and it makes `ptr<T>` fall out as the pointer level's spelling of
        // a question every level can now be asked
        //
        // **two encodings, one question.** where the flag *is* the implementation - a pointer, a class
        // handle, a weak - it answers from the flag. where `T?` needs a tagged pair it answers from the
        // layout, because there the optional is a type and the flag would be a second copy of the same
        // fact for something to disagree with. asked here rather than at the 33 readers, every one of
        // which means "may this be absent"
        //
        // the layout half is *not* paid for by most asks: `is_wrapped_optional()` returns at
        // `has_complex_type()` for four calls in five, which is a tag comparison beside `type_flags`
        // rather than a load. measured over a whole stdlib compile, ~7k asks against 0.28s of user time
        bool is_nullable() const;

        // the type one level down. `ptr<ptr<int32>>::pointee()` is `ptr<int32>`
        const ValueType &pointee() const {
            assert(is_pointer() && _pointee);
            return *_pointee;
        }

        bool is_primitive() const {
            return kind == ValueTypeKind::t_primitive;
        }

        bool is_primitive_of_type(ValueTypePrimitive primitive) const {
            return is_primitive() && this->primitive == primitive;
        }

        bool is_void() const {
            return is_primitive_of_type(ValueTypePrimitive::t_void);
        }

        // no type has been determined yet. distinct from void, which is a type: an unknown is a
        // question nothing has answered, and every consumer reads it as "says nothing" rather
        // than as a mismatch
        bool is_unknown() const {
            return kind == ValueTypeKind::t_unknown;
        }

        bool is_struct() const {
            return kind == ValueTypeKind::t_struct;
        }

        bool is_class() const {
            return kind == ValueTypeKind::t_class;
        }

        bool is_interface() const {
            return kind == ValueTypeKind::t_interface;
        }

        bool is_enum() const {
            return kind == ValueTypeKind::t_enum;
        }

        // a named user type of any storage class - exactly the kinds get_complex_type() answers
        // for. this is what a *member* question wants: `->x` resolves the same property table either
        // way, and the difference is only in how codegen reaches it. asking is_struct() where this
        // was meant is what made every class member read type as void
        //
        // an interface is one of these, which is what gives it member lookup, mangling and rendering
        // for free. so this is deliberately **not** the question "does this have a layout" - use
        // has_property_layout() below for that. the two were one predicate while there were only two
        // kinds, and every reader that meant the second one has to say so now
        bool has_complex_type() const {
            return is_struct() || is_class() || is_interface() || is_enum();
        }

        // a named user type that has *properties* - a place a `->x` can be stored in and a thing
        // codegen can lower to an llvm struct. an interface declares requirements and no storage, so
        // it is excluded: this is the half of the old has_complex_type() that meant "a layout"
        //
        // an enum is one: `__tag` and its payload fields are ordinary properties, which is the whole
        // of what lets member access, TBAA, debug info, the copy taxonomy and the teardown reach an
        // enum with no arm of their own. what an enum does not admit is a property the *author* writes,
        // and that is a refusal at its declaration rather than an answer here
        bool has_property_layout() const {
            return is_struct() || is_class() || is_enum();
        }

        bool is_numeric_type() const {
            if (!is_primitive()) {
                return false;
            }

            switch (this->primitive) {
            case ValueTypePrimitive::t_int8:
            case ValueTypePrimitive::t_int16:
            case ValueTypePrimitive::t_int32:
            case ValueTypePrimitive::t_int64:
            case ValueTypePrimitive::t_uint8:
            case ValueTypePrimitive::t_uint16:
            case ValueTypePrimitive::t_uint32:
            case ValueTypePrimitive::t_uint64:
            case ValueTypePrimitive::t_usize:
            case ValueTypePrimitive::t_isize:
            case ValueTypePrimitive::t_float32:
            case ValueTypePrimitive::t_float64:
                return true;

            default:
                return false;
            }
        }

        bool is_floating_type() const {
            if (!is_primitive()) {
                return false;
            }

            switch (this->primitive) {
            case ValueTypePrimitive::t_float32:
            case ValueTypePrimitive::t_float64:
                return true;

            default:
                return false;
            }
        }

        bool is_signed_integer() const {
            if (!is_primitive()) {
                return false;
            }

            switch (this->primitive) {
            case ValueTypePrimitive::t_int8:
            case ValueTypePrimitive::t_int16:
            case ValueTypePrimitive::t_int32:
            case ValueTypePrimitive::t_int64:
            case ValueTypePrimitive::t_isize:
                return true;

            default:
                return false;
            }
        }

        bool is_unsigned_integer() const {
            if (!is_primitive()) {
                return false;
            }

            switch (this->primitive) {
            case ValueTypePrimitive::t_uint8:
            case ValueTypePrimitive::t_uint16:
            case ValueTypePrimitive::t_uint32:
            case ValueTypePrimitive::t_uint64:
            case ValueTypePrimitive::t_usize:
                return true;

            default:
                return false;
            }
        }

        bool is_integer_type() const {
            return is_signed_integer() || is_unsigned_integer();
        }

        bool is_boolean_type() const {
            return is_primitive() && primitive == ValueTypePrimitive::t_bool;
        }

        bool will_fit_into(ValueType other) const;

        bool is_same_size(ValueType other) const;

        // a pointer has no primitive of its own - reaching here with one means a caller wanted
        // the pointee and forgot to say so. assert rather than silently answering t_void, which
        // used to reach LLVM as PointerType::get(voidTy) and assert far from the actual mistake
        inline ValueTypePrimitive get_primitive_type() const {
            assert(!is_pointer() && "use value_type_of() / pointee() to reach through a pointer");
            return primitive;
        }

        // compare two types
        //
        // one arm per kind and *no* catch-all tail: a kind added without an arm here would compare
        // unequal to itself, which is silent and poisons everything downstream - the type registry's
        // interning (equality *is* the identity it dedupes on), argument_fit's exact arm, and
        // coerce_value's passthrough. the switch makes the compiler ask for the arm instead
        bool operator==(const ValueType &other) const {
            // first check if the kinds are different
            if (kind != other.kind) {
                return false;
            }

            // check type flags (const, pointer, etc.)
            if (type_flags != other.type_flags) {
                return false;
            }

            switch (kind) {
                case ValueTypeKind::t_primitive:
                    return primitive == other.primitive;

                // a pointer is structural: same nullability (already covered by the flag check
                // above) and the same pointee, all the way down
                //
                // a weak shares the arm because it shares the representation - one recursive level in
                // `_pointee` - and structural equality is right for the same reason: `weak<Foo>` carries
                // no identity beyond the class it names
                case ValueTypeKind::t_pointer:
                case ValueTypeKind::t_weak:
                    return *_pointee == *other._pointee;

                // for struct, class, interface and enum types, compare the complex type pointers
                // two named types are equal if they point to the same ComplexType, so an
                // instantiation is a distinct type from its template and from another instantiation
                case ValueTypeKind::t_struct:
                case ValueTypeKind::t_class:
                case ValueTypeKind::t_interface:
                case ValueTypeKind::t_enum:
                    return _complex_type == other._complex_type;

                // structural, like a pointer: a signature carries no identity of its own, so two
                // separately written `function<void(int32)>`s are one type
                case ValueTypeKind::t_callable:
                    return signatures_are_equal(*this, other);

                // identity is the declaration itself, mirroring how struct/class compare their
                // ComplexType. so the T of `struct Box<T>` is not the A of `struct Pair<A, B>`
                // even though both are the first parameter of their owner
                case ValueTypeKind::t_generic:
                    return _type_param == other._type_param;

                case ValueTypeKind::t_unknown:
                    return true;
            }

            return false;
        }

        std::string get_mangled_name() const;

        std::string get_type_desciption() const;

    private:
        // operator== is inline and CallableSignature is not complete there, so the comparison is one
        // hop out of line. keeping operator== in the header is worth the hop: it is on the hot path of
        // every overload match and every interning lookup
        static bool signatures_are_equal(const ValueType &a, const ValueType &b);

        // defaulted so a default-constructed ValueType is a well-defined `unknown`
        // rather than carrying indeterminate kind/primitive
        ValueTypeKind kind = ValueTypeKind::t_unknown;
        ValueTypePrimitive primitive = ValueTypePrimitive::t_void;
        uint8_t type_flags = 0;
        ComplexType *_complex_type = nullptr;

        // for the t_generic kind: the declaration this type refers to. a pointer rather than an
        // ordinal, so the parameter's name, constraint and declaration site travel with every
        // use of it, and so parameters of different owners are distinct types
        const TypeParamDecl *_type_param = nullptr;

        // for the t_pointer kind: the type one level down. shared rather than interned because
        // a pointer carries no state beyond its pointee, so structural equality is enough -
        // unlike ComplexType, which is a mutable, property-carrying object that must be shared
        // by identity. shared_ptr keeps ValueType cheap to copy, which everything relies on
        //
        // also the t_weak kind's target, which is the same shape of question - one recursive level with
        // no identity of its own. reached through weak_target() rather than pointee(), so a reader
        // cannot cross from one kind to the other without saying which it meant
        std::shared_ptr<const ValueType> _pointee = nullptr;

        // for the t_callable kind: the signature. shared and structurally compared, for the same
        // reason _pointee is
        std::shared_ptr<const CallableSignature> _signature = nullptr;

        ValueType(ValueTypeKind kind, ValueTypePrimitive primitive) : kind(kind), primitive(primitive) {}
        ValueType(ValueTypeKind kind, ComplexType *complex_type) :
            kind(kind),
            primitive(ValueTypePrimitive::t_complex),
            _complex_type(complex_type)
        {}
        ValueType(ValueTypeKind kind, const TypeParamDecl *param) :
            kind(kind), primitive(ValueTypePrimitive::t_void), _type_param(param) {}
    };

    // held by shared_ptr off a ValueType and compared *structurally*, for the reason the pointee is:
    // it carries no mutable state and no identity of its own, so two independently written
    // `function<void(int32)>`s have to be one type or a callback could never be passed anywhere. that
    // is the whole difference from ComplexType, which is shared by pointer identity because it is a
    // mutable, property-carrying object
    struct CallableSignature
    {
        ValueType return_type;
        std::vector<ValueType> parameter_types;

        bool operator==(const CallableSignature &other) const;
    };

    inline ValueType ValueType::make_callable(ValueType return_type, std::vector<ValueType> parameter_types)
    {
        ValueType type(ValueTypeKind::t_callable, ValueTypePrimitive::t_void);
        type._signature = std::make_shared<const CallableSignature>(
            CallableSignature { std::move(return_type), std::move(parameter_types) });
        return type;
    }

    inline const CallableSignature &ValueType::signature() const
    {
        assert(is_callable() && "only a callable has a signature");
        return *_signature;
    }

    inline bool ValueType::signatures_are_equal(const ValueType &a, const ValueType &b)
    {
        // one signature shared by both, which is what a copy of a ValueType produces - and a copy is
        // how nearly every comparison gets its operands. the structural compare below is recursive over
        // the return type and every parameter, so short-circuiting it is worth a pointer test
        if (a._signature == b._signature) {
            return true;
        }

        return a.signature() == b.signature();
    }

    class ComplexType
    {
    public:
        struct Property
        {
            size_t index;
            std::string name;
            ValueType type;

            // reachable only from inside the declaring type. here as well as on the VarDeclNode for
            // `kind`'s reason: an *instantiation* has properties and no declaration nodes, so this is
            // the only thing that can answer for `mem::buffer<int32>` what `mem::buffer` said
            bool is_private = false;
        };

        std::optional<std::string> name;

        // stack value type or reference counted heap type - the one bit that separates `struct` from
        // `class`. it lives here rather than only on the ValueType because an *instantiation* has a
        // ComplexType and no declaration node, so this is the only thing that can answer for
        // `Box<int32>` what kind `Box` was declared as. ValueType::make_complex reads it, which is
        // what makes a mismatched (kind, ComplexType) pair unconstructible
        ComplexTypeKind kind = ComplexTypeKind::t_struct;

        // written `#[unique]`: **exactly one value may name this storage.** copying is refused, moving
        // transfers it, destruction ends it - which is what makes two live values of the type provably
        // two regions, the one fact AST::path_overlap cannot get any other way.
        //
        // here beside `kind` and for its reason: an *instantiation* has a ComplexType and no
        // declaration node, so this is the only thing that can answer for `buffer<int32>` what
        // `buffer` was declared as. AST::TypeRegistry::get_or_create_instantiation carries it across
        //
        // deliberately not on ValueType: it is a property of the declared type, identical for every
        // spelling of it, and a flag on the interning identity would fork `buffer<T>` in two
        bool is_unique = false;

        // **this layout is a `T?` whose payload has no null value of its own** - two properties,
        // `__has` then `__value`, minted by AST::TypeRegistry::get_or_create_optional.
        //
        // beside `kind` and `is_unique` and for their reason. an optional *is* a struct for every layout
        // purpose - an aggregate with properties, member access, TBAA, debug info - and exactly one
        // thing differs: its teardown and its copy have to test `__has` first, where a struct's walk
        // every property unconditionally. so it is a flag on the layout rather than a fourth
        // ComplexTypeKind, and the eleven `is_struct()` sites are untouched.
        //
        // **the point of it being a layout at all.** nullability is a per-level flag wherever that flag
        // *is* the implementation - a pointer, a class handle, a weak, where a null address already means
        // absent and `T?` costs nothing. over anything else `T?` is a tagged pair, and while that pair was
        // a flag too, every question about it was answered about the payload instead: needs_destruction
        // and classify_copy read through it, every synthesized teardown and copy declaration stripped it,
        // and no call site branched on it - so a `string?` could not be declared, passed or returned.
        // As a layout it is an ordinary owning type and all of that machinery applies unchanged
        bool is_optional = false;

        // **who may name this type**, and where it was written to answer that against.
        //
        // here beside `kind` and `is_unique`, and for exactly their reason: an *instantiation* has a
        // ComplexType and no declaration node, so this is the only thing that can answer for `Box<int32>`
        // what `Box` was declared as - and TypeRegistry::get_or_create_instantiation carries both across.
        //
        // it also answers for the one shape that has a type but no type *name* to check:
        // `Hidden(1)` builds a value without spelling `Hidden` in type position anywhere, so the refusal
        // has to be reachable from the constructor's own declaration. AST::enclosing_type_of gives that
        // constructor's ComplexType, and this is what makes the answer available there
        //
        // settled in the *type names* pass, a pass earlier than every other declaration settles its own:
        // the declaration pass resolves a property's type against a type declared in another file, so
        // asking there would depend on which file was walked first
        Visibility visibility = Visibility::t_public;

        DeclarationOrigin declared_in;

        bool is_class_kind() const {
            return kind == ComplexTypeKind::t_class;
        }

        bool is_interface_kind() const {
            return kind == ComplexTypeKind::t_interface;
        }

        bool is_enum_kind() const {
            return kind == ComplexTypeKind::t_enum;
        }

        // has properties, and so a layout codegen can lower. false for an interface, which declares
        // requirements and stores nothing - the mirror of ValueType::has_property_layout()
        bool has_property_layout() const {
            return kind != ComplexTypeKind::t_interface;
        }

        // the namespace the type was declared in, null when unknown or root. instantiations
        // inherit it from their template, so a mangled name can always be fully qualified.
        // non-const because a type *declares into* it: AST::member_surface_namespace hangs the type's
        // nested types and constants off this very object rather than off a path from the root
        Namespace *ast_namespace = nullptr;

        // the type this one is declared *inside*, null for a top-level type. it is part of this type's
        // identity, not decoration: a nested type is in no namespace, so without it `string::view` and
        // any other struct's `view` would render the same in a diagnostic and - far worse - produce the
        // same mangled_token(), which is what interning and every method symbol key on
        const ComplexType *owner_type = nullptr;

        // this template's own generic parameters (the T, U in `struct Foo<T, U>`), owned by the
        // collector's TypeParamRegistry. empty for non-generics and for instantiations. always
        // append through add_type_parameter, which keeps the ordinal and owner consistent
        std::vector<TypeParamDecl *> type_parameters;
        ComplexType *template_ref = nullptr;  // null for templates; points to original for instantiations
        std::vector<ValueType> instantiation_args;  // empty for non-instantiated

        ComplexType() = default;
        ComplexType(std::string name) : name(name) {}

        bool is_named() const {
            return name.has_value();
        }

        // the type name prefixed with its namespace path ("a::b::Foo"), the bare name when the
        // type sits in the root namespace or none is known
        std::string namespaced_name() const;

        // the length prefixed, namespace qualified token this type contributes to a mangled
        // symbol name. unambiguous and free of characters that are invalid in symbols
        std::string mangled_token() const;

        bool is_generic() const {
            return !type_parameters.empty();
        }

        bool is_instantiated() const {
            return template_ref != nullptr;
        }

        // the type a question about the *declaration* is asked of: the template for an instantiation,
        // and this type itself otherwise. an instantiation carries the layout of `Box<int32>` but
        // nothing that was written - no methods, no destructor, not even the name a constructor of it
        // is spelled with, since that is `Box`
        //
        // the one spelling of that redirect. AST::find_member_functions and AST::find_destructor are
        // it asked about a different store, and it is the contract that keeps TypeRegistry ignorant
        // of members
        const ComplexType *template_or_self() const {
            return is_instantiated() ? template_ref : this;
        }

        // appends a type parameter, stamping this type as its owner. asserts the ordinal matches
        // the position it lands in, so the ordinal can never drift from the list order
        // defined out of line because it needs TypeParamDecl complete
        void add_type_parameter(TypeParamDecl *param);

        // true if `type` is a type parameter declared by this very type. the single place that
        // knows how a t_generic ValueType maps back to a declaration
        bool declares_type_param(const ValueType &type) const;

        void add_property(const std::string &name, ValueType type, bool is_private = false) {
            // on a template, a `T`-typed property must reference one of this type's own declared
            // parameters. instantiations carry no type_parameters of their own, so the check only
            // applies while a template is being built
            if (type.is_type_param() && !is_instantiated()) {
                assert(declares_type_param(type));
            }
            _property_map[name] = _properties.size();
            _properties.push_back(Property { _properties.size(), name, type, is_private });
        }

        // retypes a property that is already there. the *layout* is fixed by the order properties were
        // appended in and is not touched - only what the slot holds
        //
        // exists for one caller, a closure environment: its properties are the types of the places the
        // body captured, and a captured place is not typed until the monomorphizer has settled the call
        // its variable was inferred from. so the property is appended when the capture is *found*, at
        // parse time, and its type re-derived when the place finally has one - the same stale-then-
        // re-derived shape a variable's own inferred type has. a declared struct never needs this: its
        // properties are written with their types
        void set_property_type(size_t index, ValueType type) {
            _properties.at(index).type = type;
        }

        bool has_property(const std::string &name) const {
            return _property_map.find(name) != _property_map.end();
        }

        bool has_property(size_t index) const {
            return index < _properties.size();
        }

        // the property a name denotes, or null. resolves the name once, so a caller that needs
        // both the index and the type (codegen's GEP does) pays for one lookup rather than a
        // has/get pair plus a linear scan to recover the index
        const Property *find_property(const std::string &name) const {
            auto it = _property_map.find(name);
            return it == _property_map.end() ? nullptr : &_properties[it->second];
        }

        const ValueType &get_property_type(const std::string &name) const {
            return _properties.at(_property_map.at(name)).type;
        }

        const ValueType &get_property_type(size_t index) const {
            return _properties.at(index).type;
        }

        const Property &get_property(size_t index) const {
            return _properties.at(index);
        }

        size_t property_count() const {
            return _properties.size();
        }

        // the member functions declared on this type, in declaration order. a name denotes an
        // overload set here exactly as it does in the FunctionRegistry, so this is a flat list
        // rather than a name map - AST::find_member_functions does the matching.
        //
        // only a *template* (or a plain non-generic struct) ever holds methods: an instantiation
        // gets its members by instantiating the template's, per call site, so a lookup on
        // `Box<int32>` redirects through template_ref rather than finding a list of its own
        // appends a method. no dedup here: the two-pass idempotency belongs to
        // FunctionRegistry::claim_declaration_site, which returns early before ever reaching this -
        // a second guard would only invite a reader to believe the vector is the one that owns it
        void add_method(FunctionDeclNode *decl) {
            _methods.push_back(decl);
        }

        const std::vector<FunctionDeclNode *> &methods() const {
            return _methods;
        }

        // the **static** functions declared on this type, in declaration order - the same flat
        // overload set the methods above are, matched by AST::find_static_functions.
        //
        // **a second list rather than a flag on the first**, because the two are reached by different
        // syntax and neither may answer for the other: a method is found through a receiver, a static
        // by naming the type. sharing one list would make `$box->make(1)` resolve to a declaration
        // that never declared an args[0], and the check that stopped it would be a filter every reader
        // of methods() had to remember - which is the same reason a method is not in _by_name
        //
        // the template_ref story is identical: only a template holds these, and a lookup on
        // `result<int32, string>` redirects through it. what differs is what binds the owner's type
        // parameters at the call site - a method has a receiver, a static has AST::static_owner_bindings
        void add_static_method(FunctionDeclNode *decl) {
            _static_methods.push_back(decl);
        }

        const std::vector<FunctionDeclNode *> &static_methods() const {
            return _static_methods;
        }

        // the **static** properties declared on this type, in declaration order - `static int32 $count`.
        //
        // **deliberately not in `_properties`**, which is the *layout*: every reader of that vector means
        // "the fields a value of this type carries", and they are many - the field-wise constructor, the
        // copy, the deinit, `size_of`, every codegen GEP. a static carries none of that, having one
        // global for the whole type rather than a slot in each value, so a flag on the property would
        // be a filter each of those readers had to remember. the one that forgot would seat a global
        // into a struct's third field
        //
        // the *declaration* is stored, as a method is: it holds the initializer expression, and
        // AST::OwnershipPass clones that per instantiation into the body it synthesizes
        void add_static_property(VarDeclNode *decl) {
            _static_properties.push_back(decl);
        }

        const std::vector<VarDeclNode *> &static_properties() const {
            return _static_properties;
        }

        // this type's static property of that name, and its index, or nullopt. the index is half the
        // identity of the global it names - see AST::static_property_symbol - so the two are answered
        // together rather than by a lookup and then a search
        std::optional<std::pair<size_t, VarDeclNode *>> find_static_property(const std::string &name) const;

        // **the cases of an enum, in declaration order** - `case timeout(int32 $after)`.
        //
        // here beside `kind` and the two member lists, and for exactly their reason: an *instantiation*
        // has a ComplexType and no declaration node, so this is the only thing that can answer for
        // `result<int32, string>` what `result` wrote. TypeRegistry::get_or_create_instantiation carries
        // it across, as it carries `kind` and `is_unique`
        //
        // **the case is not the constructor.** `X::timeout(30)` resolves through `_static_methods` like
        // any other static, which is what AST::find_static_functions' refusal to ask what kind the owner
        // is already buys. what this table is for is the two questions a call cannot answer: which
        // discriminant a *pattern* names, and which properties a case's payload occupies - neither of
        // which is a function
        struct EnumCase
        {
            size_t ordinal;
            std::string name;

            // what `__tag` holds for this case. the ordinal, or the written value for an integer-backed
            // enum - which is why it is stored rather than derived: `case not_found = 404` is a case
            // whose ordinal is 1
            int64_t discriminant;

            // the written value of a `: string` backed enum. the discriminant stays the ordinal there,
            // because a string cannot be a discriminant and every enum value would otherwise carry one
            std::optional<std::string> backing_string;

            // where this case's payload sits in `_properties`, and how wide it is. a half-open range
            // rather than a list of indices: the layout synthesis appends a case's fields together, so
            // contiguity is a fact about how it is built and not a coincidence to be preserved by hand
            size_t first_payload_property = 0;
            size_t payload_field_count = 0;

            // the names as the author wrote them. read by the diagnostics and by the pattern binder,
            // which needs the arity - a pattern binding the wrong number of names is refused against
            // this rather than against the constructor, a pattern not being a call
            std::vector<std::string> payload_field_names;

            bool has_payload() const {
                return payload_field_count > 0;
            }
        };

        void add_enum_case(EnumCase entry) {
            _enum_cases.push_back(std::move(entry));
        }

        const std::vector<EnumCase> &enum_cases() const {
            return _enum_cases;
        }

        // the case that name denotes, or null. **the one lookup** - a linear walk, as the static
        // property's is, since a case list is short and a second index would be a second thing to carry
        // across an instantiation
        const EnumCase *find_enum_case(const std::string &name) const;

        // the type written after the `:` of a backed enum - an integer primitive or `string`. absent for
        // a plain enum and for one with payload cases, the two being mutually exclusive at the
        // declaration. what reads it is the synthesized `value()` and the choice of `__tag`'s own type
        std::optional<ValueType> enum_backing;

        // the types declared *inside* this one, by name. `string::view` is reached through its owner
        // and lives in no namespace at all, which is the same decision that keeps a method out of
        // FunctionRegistry::_by_name: no namespace path a user can write reaches it, so two structs
        // may each declare a `view` without colliding
        //
        // a map rather than the flat list methods use, because a type name denotes exactly one type -
        // there is nothing to overload it on. only a *template* (or a plain non-generic struct) holds
        // one, which is why a nested type inside a generic owner is refused at its declaration site
        //
        // the *declaration* is what is stored, as it is for a method beside it: the parse passes reach
        // this node again to reconcile on it, and a type site wants the node, not a layout
        void add_member_type(const std::string &name, TypeDeclNode *decl) {
            _member_types[name] = decl;
        }

        // this type's own nested declaration of that name, or null. **the one lookup** - see the note
        // in ASTMemberLookup.h for why this one needs no template_ref redirect beside it
        TypeDeclNode *find_member_type_decl(const std::string &name) const {
            auto it = _member_types.find(name);
            return it == _member_types.end() ? nullptr : it->second;
        }

        // this type's destructor, or null. at most one, so a slot rather than an overload set -
        // it takes no parameters, which leaves nothing to overload on
        //
        // on the *type* rather than on TypeDeclNode for the same reason methods are: an
        // instantiation has a ComplexType and no TypeDeclNode, and the drop sites the ownership
        // pass inserts are keyed on the type of the value being destroyed. only a template ever
        // holds one - AST::find_destructor redirects an instantiation through template_ref, as
        // AST::find_member_functions does
        void set_destructor(FunctionDeclNode *decl) {
            _destructor = decl;
        }

        FunctionDeclNode *destructor() const {
            return _destructor;
        }

        // this type's copy constructor - the constructor taking one non-nullable borrow of its own
        // type - or null. at most one, so a slot beside the destructor above, and reached by type for
        // the same reason: the copy sites AST::OwnershipPass inserts know the type of the value being
        // copied and nothing else. unlike the destructor above, an *instantiation* can hold one of its
        // own: a written copy constructor is a member and lives on the template, but a synthesized one
        // is built per concrete type, because whether the compiler can write it at all depends on the
        // property types. AST::find_copy_constructor is where the two are reconciled
        //
        // the one thing that makes it unlike a destructor: **the declaration is also in the ordinary
        // overload set.** a destructor is deliberately kept out of FunctionRegistry::_by_name because
        // `$x->destructor()` is not a call anyone may write - but `Foo($a)` is written all the time
        // and already resolves, and it has to resolve to *this* declaration. so this slot says which
        // of a type's constructors the copy is, and takes nothing away from the set
        void set_copy_constructor(FunctionDeclNode *decl) {
            _copy_constructor = decl;
        }

        FunctionDeclNode *copy_constructor() const {
            return _copy_constructor;
        }

        // the synthesized function that tears a value of this type down - this type's own destructor
        // first, then each owning property. a class reaches it from its release thunk when the strong
        // count hits zero, a struct from the drop AST::OwnershipPass wrote at the end of the value's
        // scope, and it is one function because it is one teardown: what a class destroys at zero and
        // what a struct destroys at a scope end can never disagree.
        //
        // null in two cases that are not the same. **the value owns nothing**, so there is nothing to
        // tear down - for a class that is a release which decrements and frees. or, for a **struct**,
        // its declared destructor is the whole teardown and needs no body wrapped around it, so
        // AST::OwnershipPass::ensure_deinit answers that destructor and leaves this slot alone. a class
        // is never that second case, because *this slot* is what codegen reads and an instantiation's
        // find_destructor answers the template's declaration, which has no symbol.
        //
        // deliberately no template_or_self redirect, unlike the destructor above: the body is built per
        // concrete type, since which properties own anything is what an instantiation decides
        void set_deinit(FunctionDeclNode *decl) {
            _deinit = decl;
        }

        FunctionDeclNode *deinit() const {
            return _deinit;
        }

        // the methods that declare an implicit conversion *from* this type - the ones the user marked
        // `#[implicit]`.
        //
        // A value handed to a parameter of one of their return types is converted by a call to it, with
        // nothing written at the call site. AST::find_implicit_conversion is the lookup, and
        // AST::argument_fit ranks the result last of the real ranks - below even a borrow of a
        // temporary, so an overload taking the owning type always beats one taking the window.
        //
        // **declarations only, never the target type.** the declaration's return type already *is* the
        // target. A second copy of it here would be a member field on ComplexType carrying a type, which
        // TypeRegistry::derive_instantiation does not fill - so `array<int32>` would keep saying
        // `slice<T>`. Holding the declaration means this slot behaves exactly like _methods, which an
        // instantiation reaches through the template_or_self redirect rather than owning a copy of.
        //
        // A list, rather than the single slot the destructor and the copy constructor get. A type may
        // offer a window over itself in more than one shape, and a list puts the duplicate check on the
        // right question: not "is the slot taken" but "is there already one to *this* target", which is
        // what the user needs told. Only valid declarations are ever pushed, so no reader re-filters
        void add_implicit_conversion(FunctionDeclNode *decl) {
            _implicit_conversions.push_back(decl);
        }

        const std::vector<FunctionDeclNode *> &implicit_conversions() const {
            return _implicit_conversions;
        }

        // the interfaces this type declared it conforms to - the `: Drawable, contract::iterable<E>` clause.
        // what AST::conforms_to answers from, which is what a type-parameter constraint and an `instanceof`
        // over an interface read, and what the runtime conformance table is built from
        //
        // **types, not declarations**, which makes this the one member field here that
        // TypeRegistry::derive_instantiation has to substitute onto an instantiation, alongside the
        // properties.
        //
        // That is the opposite of _implicit_conversions above, which deliberately stores declarations so
        // no ValueType on this class ever needs substituting. The difference is that a conversion's
        // target is already spelled by the declaration's return type, while a conformance has no
        // declaration of its own at all. `struct Bag<E> : contract::iterable<E>` has to become
        // `contract::iterable<int32>` on `Bag<int32>`, or a constraint would be checked against a
        // template
        //
        // only valid, deduplicated entries are ever pushed - parse_typedecl refuses a non-interface and
        // a repeat at the declaration - so no reader re-filters
        void add_conformance(ValueType interface) {
            _conformances.push_back(std::move(interface));
        }

        const std::vector<ValueType> &conformances() const {
            return _conformances;
        }

        // the associated types this *interface* declares - the `type Iter : contract::iterator<V>` in an
        // interface body. a type the implementor chooses and the interface only constrains.
        //
        // **a list of its own, deliberately not appended to `type_parameters`.** that list is the type's
        // *arity*: get_or_create_instantiation asserts on it, parse_generic_application checks a written
        // application against it, and TypeSubstitution::positional is positional over exactly it. an entry
        // there would change the identity of every `contract::iterable<...>` a program can write
        //
        // **declarations, never types** - so TypeRegistry::derive_instantiation has nothing to substitute
        // here, which is the same choice _implicit_conversions makes and the opposite of _conformances.
        // what an implementor *binds* one to is not stored anywhere at all: it is knowable only after the
        // implementor's methods exist, which is strictly later than get_or_create_instantiation's
        // staleness test can notice, so AST::conformance_bindings derives it on every ask
        void add_associated_type(TypeParamDecl *decl);

        const std::vector<TypeParamDecl *> &associated_types() const {
            return _associated_types;
        }

        // by name, for the parser: pass 2 and pass 3 both walk the body, and the declaration has to be
        // *reused* rather than minted twice. equality on a type parameter is TypeParamDecl* identity, so
        // two decls make pass 2's `Iter` compare unequal to pass 3's and every conformance reports unmet
        // with a message naming two identical-looking types
        TypeParamDecl *find_associated_type(const std::string &name) const;

        // there is deliberately no way to mint a substituted copy of a layout here.
        // TypeRegistry::get_or_create_instantiation is the *only* thing that produces a ComplexType for
        // an instantiation, because struct equality is ComplexType* identity and a second minter is a
        // second identity for one type. TypeDeclNode::clone used to be that second minter; it now shares
        // the declaration instead

    private:
        std::vector<Property> _properties;
        std::unordered_map<std::string, size_t> _property_map;
        std::vector<FunctionDeclNode *> _methods;
        std::vector<FunctionDeclNode *> _static_methods;
        std::vector<VarDeclNode *> _static_properties;
        std::unordered_map<std::string, TypeDeclNode *> _member_types;
        FunctionDeclNode *_destructor = nullptr;
        FunctionDeclNode *_copy_constructor = nullptr;
        FunctionDeclNode *_deinit = nullptr;
        std::vector<FunctionDeclNode *> _implicit_conversions;
        std::vector<ValueType> _conformances;
        std::vector<TypeParamDecl *> _associated_types;
        std::vector<EnumCase> _enum_cases;

        friend class TypeRegistry;  // allow TypeRegistry to access _properties
    };

    // the three that read the *layout*, so they sit here rather than in the class body: ComplexType is
    // declared after ValueType, and a member function cannot reach into an incomplete type. still inline,
    // for the reason operator== stays in the header - these are asked all over resolution and codegen, and
    // out of line each ask would be a call to read one tag
    inline bool ValueType::is_wrapped_optional() const
    {
        return has_complex_type() && get_complex_type()->is_optional;
    }

    inline const ValueType &ValueType::optional_payload() const
    {
        assert(is_wrapped_optional() && "optional_payload over a type that is not a tagged optional");
        return get_complex_type()->get_property_type(k_optional_value_index);
    }

    inline bool ValueType::is_nullable() const
    {
        return (type_flags & static_cast<uint8_t>(ValueTypeFlags::t_nullable)) || is_wrapped_optional();
    }

};  // namespace AST

// hash support for ValueType to be used in unordered containers
namespace std
{
    template<> struct hash<AST::ValueType> {
        size_t operator()(const AST::ValueType &vt) const {
            size_t h = static_cast<size_t>(vt.get_kind()) ^ vt.get_type_flags();
            if (vt.is_primitive()) h ^= static_cast<size_t>(vt.get_primitive_type());
            else if (vt.has_complex_type()) h ^= reinterpret_cast<size_t>(vt.get_complex_type());
            else if (vt.is_type_param()) h ^= reinterpret_cast<size_t>(vt.get_type_param());
            // mixed rather than xor'd: a bare xor of the pointee hash would make ptr<int32>
            // collide with int32, since the primitive component is identical
            else if (vt.is_pointer()) h ^= (*this)(vt.pointee()) + 0x9e3779b9 + (h << 6) + (h >> 2);
            // the same mix over the class it names. the kind is already in `h`, so `weak<Foo>` and
            // `ptr<Foo>` do not collide despite hashing one recursive level the same way
            else if (vt.is_weak()) h ^= (*this)(vt.weak_target()) + 0x9e3779b9 + (h << 6) + (h >> 2);
            // structural, so the hash has to be too - mixed for the reason above, and over the
            // return type as well as the parameters, since `function<int32()>` and
            // `function<void()>` differ only there
            else if (vt.is_callable()) {
                h ^= (*this)(vt.signature().return_type) + 0x9e3779b9 + (h << 6) + (h >> 2);
                for (const auto &param : vt.signature().parameter_types) {
                    h ^= (*this)(param) + 0x9e3779b9 + (h << 6) + (h >> 2);
                }
            }
            return h;
        }
    };
};  // namespace std

namespace AST
{

    // hash support for tuple used in TypeRegistry
    struct TypeRegistryKeyHash
    {
        size_t operator()(const std::tuple<ComplexType*, std::vector<ValueType>> &key) const {
            size_t h1 = std::hash<ComplexType*>{}(std::get<0>(key));
            size_t h2 = 0;
            for (const auto &vt : std::get<1>(key)) {
                h2 ^= std::hash<AST::ValueType>{}(vt) + 0x9e3779b9 + (h2 << 6) + (h2 >> 2);
            }
            return h1 ^ (h2 << 1);
        }
    };

    // maps type-parameter declarations to the (possibly still-generic) types they stand for
    // used identically for function and struct type parameters
    //
    // keyed by declaration rather than by position, which is what makes a *partial* substitution
    // meaningful: substituting a generic member of a generic owner binds the owner's parameters
    // and leaves the member's own alone. composing two substitutions is concatenating bindings
    //
    // a vector rather than a hash map because a generic declares one to three parameters in
    // practice, so a linear scan wins and the iteration order stays deterministic
    struct TypeSubstitution
    {
        std::vector<std::pair<const TypeParamDecl *, ValueType>> bindings;

        TypeSubstitution() = default;

        // binds `params` to `args` positionally, the shape both a call site's resolved type
        // arguments and a generic application arrive in. asserts the arity matches: this is the
        // one place that check lives now that substitute_type tolerates unbound parameters
        static TypeSubstitution positional(const std::vector<TypeParamDecl *> &params, const std::vector<ValueType> &args) {
            assert(params.size() == args.size());

            TypeSubstitution subst;
            subst.bindings.reserve(params.size());
            for (size_t i = 0; i < params.size(); i++) {
                subst.bindings.emplace_back(params[i], args[i]);
            }
            return subst;
        }

        // rebinding an already bound parameter replaces it, so a later, better inference wins
        void bind(const TypeParamDecl *param, const ValueType &type) {
            assert(param);
            for (auto &binding : bindings) {
                if (binding.first == param) {
                    binding.second = type;
                    return;
                }
            }
            bindings.emplace_back(param, type);
        }

        // null when the parameter is not bound here, which substitute_type reads as "leave it"
        const ValueType *lookup(const TypeParamDecl *param) const {
            for (const auto &binding : bindings) {
                if (binding.first == param) {
                    return &binding.second;
                }
            }
            return nullptr;
        }

        bool covers(const TypeParamDecl *param) const {
            return lookup(param) != nullptr;
        }

        bool empty() const {
            return bindings.empty();
        }

        size_t size() const {
            return bindings.size();
        }
    };

    // caches and owns every generic instantiation. Interning is by (template, args) identity,
    // which is exactly what ValueType::operator== relies on (struct/class equality is ComplexType*
    // pointer identity): equal argument lists always yield the same ComplexType*
    class TypeRegistry
    {
    public:
        // register a template so that (template, {}) resolves back to the template itself.
        ComplexType *register_template(ComplexType *tmpl) {
            assert(tmpl->is_generic() && !tmpl->is_instantiated());
            auto key = std::make_tuple(tmpl, std::vector<ValueType>{});
            auto [it, inserted] = _instantiations.emplace(key, tmpl);
            return it->second;
        }

        // intern the instantiation of `tmpl` with `args`, substituting the template's properties
        // through `args`. Implemented in ASTValueType.cpp so it can call the unified substitute_type
        ComplexType *get_or_create_instantiation(ComplexType *tmpl, const std::vector<ValueType> &args);

        // a type the *compiler* declares, with no TypeDeclNode behind it and no template it instantiates
        // - a closure's capture environment is the one caller today. it is nothing but a name, a kind and
        // whatever properties the caller appends
        //
        // it lives here because this is where a ComplexType's lifetime and the pointer-identity contract
        // already live: `_owned` keeps it alive for the whole compilation, which is what lets a ValueType
        // hold it by raw pointer. nothing downstream needs a declaration node - StructureTable keys a
        // layout on the ComplexType and create_llvm_struct_for_instance builds it from the properties
        ComplexType *create_anonymous_type(const std::string &name, ComplexTypeKind kind, Namespace *ns);

        // **`T?` where the payload has no null value of its own**, interned on the payload so the type is
        // one type however many places spell it. Two properties, `__has` then `__value`, and
        // `ComplexType::is_optional` set - see that flag for why an optional is a layout at all.
        //
        // Interned here rather than minted per spelling because ValueType equality is pointer identity on
        // the ComplexType: two `string?`s that were not the same layout would be two unrelated types, and
        // an argument would stop matching its parameter.
        //
        // **Idempotent**, like the flag it replaces: the payload of a `T?` is already a `T`, so asking for
        // `T??` hands back `T?`. There is one `null` in the language and nothing for a second level of
        // absence to mean.
        //
        // Refuses a payload that has a null representation of its own - that case is a flag and
        // ValueType::make_nullable owns it. Two answers to "is this absent" is the bug this whole shape
        // exists to remove, so the split is asserted at both ends
        ValueType get_or_create_optional(const ValueType &payload);

        // the interned concrete instantiations (Box<int>, ...), excluding the bare templates that
        // register_template maps to themselves. used by the --print-instances dump
        std::vector<ComplexType*> instantiations() const {
            std::vector<ComplexType*> result;
            for (const auto &[key, ct] : _instantiations) {
                if (ct->is_instantiated()) {
                    result.push_back(ct);
                }
            }
            return result;
        }

    private:
        std::string args_description(const std::vector<ValueType> &args) const;

        // (re-)derives everything an instantiation takes from its template through `args`: the property
        // layout, and the conformance list. one function for both, because they are the same operation
        // on the same substitution and go stale together - a generic application can be interned during
        // the declaration pass, *before* the template's own body has been walked, so an instance may
        // have to be refilled later
        //
        // re-entrant: substituting `struct Bag<E> : contract::iterable<Bag<E>>` asks this registry for
        // `Bag<int32>`, which is already in the cache but mid-derivation, and the staleness test would
        // then send it straight back in here. `_deriving` is what makes the inner call a no-op and lets
        // the outer one finish the job
        void derive_instantiation(ComplexType *inst, ComplexType *tmpl, const std::vector<ValueType> &args);

        // owns the instantiation ComplexTypes this registry creates. Templates are owned elsewhere
        // (embedded on their TypeDeclNode) and only appear here as map values, never in _owned
        std::vector<std::unique_ptr<ComplexType>> _owned;
        std::unordered_map<std::tuple<ComplexType*, std::vector<ValueType>>, ComplexType*, TypeRegistryKeyHash> _instantiations;
        std::unordered_set<ComplexType *> _deriving;

        // the tagged optionals, keyed on the payload. a separate map from `_instantiations` because an
        // optional instantiates no template - there is nothing to substitute and nothing to go stale -
        // and because the key is one type rather than a (template, args) pair
        std::unordered_map<ValueType, ComplexType *> _optionals;
    };

    // the single, unified type-substitution routine, shared by struct and function generics
    // - a type-parameter reference resolves to subst[index], carrying its const/pointer flags;
    // - a generic application (a struct/class whose ComplexType is an instantiation) has its
    //   arguments recursively substituted and is then re-interned via `registry` - this is what
    //   makes nested generics such as Foo<Bar<T>> work;
    // - primitives and already-concrete types are returned unchanged
    ValueType substitute_type(const ValueType &type, const TypeSubstitution &subst, TypeRegistry &registry);

    // true if the type is, or structurally contains, an unresolved type parameter - either directly
    // or as an argument of a generic application. after monomorphization a concrete context should be
    // free of these; anything left is a resolution bug rather than a legitimate type.
    bool contains_type_param(const ValueType &type);

    // the same walk, asked about **one** declaration rather than about any of them: does this type
    // mention `param`? a null `param` asks the coarse question above, so one call site can do either
    //
    // the asker is a declaration validating its own list - an operator refusing a parameter no
    // operand mentions, because nothing at a use site could ever bind it
    bool contains_type_param(const ValueType &type, const TypeParamDecl *param);

    // true when nothing has answered what this type is yet: unknown, void, or still mentioning a
    // type parameter that a substitution has not bound
    //
    // the single spelling of "no information", because every pass that reasons about types before
    // they are all known needs the same three-way distinction - a wrong type, a right type, and no
    // type yet. overload ranking reads it as neutral, generic inference as "cannot bind from this,
    // ask again later", and both used to say it in their own words
    inline bool is_undetermined_type(const ValueType &type) {
        return type.is_unknown() || type.is_void() || contains_type_param(type);
    }

    // the type a value-position read of `type` yields: the pointee for a pointer, the type itself
    // otherwise. exactly one level, never more - `ptr<ptr<uint8>>` reads as `ptr<uint8>`
    //
    // one primitive behind two rules that are the same rule: the auto-deref that makes a pointer
    // behave like the value it points at, and the generic decay that binds T=int32 when a
    // ptr<int32> is passed to `function box<T>(T $v)`
    ValueType value_type_of(const ValueType &type);

    // the type ultimately addressed, following every pointer level rather than one
    //
    // deliberately separate from value_type_of, which is the *read* rule and must stay at one
    // level. this is the `->` rule: a member lives on the struct, however many addresses deep
    // the base happens to be, so `ptr<ptr<Point>>` still finds Point's fields
    ValueType target_type_of(const ValueType &type);

    // true when a value of `from` may be used where `to` is expected without an explicit cast
    //
    // deliberately looser than operator==, which has to stay exact because it is the interning
    // identity for TypeRegistry and the monomorphizer's instance cache - Box<int32> and
    // Box<const int32> are different layouts even though an int32 argument satisfies a const
    // int32 parameter. the two differences here:
    //   - top level const is dropped
    //   - a non-nullable borrow T& widens to a nullable ptr<T>, never the reverse
    bool is_implicitly_convertible(const ValueType &from, const ValueType &to);

    // **may a hint for this type be handed down into an expression before it is parsed?** a
    // *parse-time* question, and deliberately not the same one AST::type_literal_at answers - that one
    // is the rule, this one is whether the rule may be applied to an operand nobody has seen yet.
    //
    // only a concrete, non-void primitive that is not a `bool` can. every other kind reaches a literal
    // as a hint that says nothing about it:
    //   - a pointer hint belongs to the expression as a whole, not to the operand - in
    //     `ptr<int> $q = $p:$ + 1` the literal is only the element offset;
    //   - a type parameter says nothing until it is substituted;
    //   - a struct or class has no conversion from a literal at all
    // in all of those the literal is typed on its own (int32 unless the value needs int64) and
    // fitted at its destination by TypeLowering::coerce_value after monomorphization, the same way
    // a `return` in a generic body is. treating them as hints instead made the autocast helpers
    // report a bogus "unexpected token" at the literal
    //
    // **`bool` is on that list and the reason is the parser's shape.** the shunting yard hands every
    // operand the same hint and cannot know the operator yet, so a `bool` destination reached the `3`
    // and the `4` of `bool $x = 3 < 4;` and retyped both - a comparison decided on garbage. the
    // destination is applied *once, to the finished expression*, in Parser::parse_expr_ref, which is
    // where `bool $a = 1;` becomes `true` and `bool $a = 3;` is refused. a bool is not a width, so
    // there is nothing for the hint to decide about an *operand* even when the finished literal
    // would have been 0 or 1
    bool can_type_a_literal(const ValueType &type);

};

#endif
