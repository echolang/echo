#ifndef ASTVALUETYPE_H
#define ASTVALUETYPE_H

#pragma once

#include <type_traits>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <unordered_map>
#include <tuple>
#include <optional>
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

    enum class ValueTypeKind {
        t_primitive,
        t_class,
        t_struct,
        t_generic,
        t_pointer,
        t_unknown
    };

    // flags apply to the level they sit on, which is what makes `ptr<const T>` (const pointee)
    // and `const ptr<T>` (const pointer) distinct types. t_nullable only ever sits on a
    // t_pointer level: set for `ptr<T>`, clear for the non-nullable borrow `T&`
    enum class ValueTypeFlags {
        t_const = 1 << 0,
        t_nullable = 1 << 1,
    };

    enum class ValueTypePrimitive {
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

    struct IntegerSize {
        uint8_t size;
        bool is_signed;

        IntegerSize(uint8_t size, bool is_signed) : size(size), is_signed(is_signed) {}

        // the bounds are built by shifting a full mask *down* rather than shifting 1 *up* by the
        // width. `1ULL << 64` is undefined behaviour, and because `size` arrives from a
        // non-inlined switch it was a runtime shift: arm64 and x86 mask the count to 6 bits, so
        // the widest unsigned type reported a maximum of 0 and rejected every literal assigned
        // to a uint64. shifting down never reaches a width-sized shift count
        int64_t get_max_negative_value() const {
            if (!is_signed) return 0;
            const unsigned bits = static_cast<unsigned>(size) * 8;
            // -2^(bits-1), negated in unsigned space because negating the int64 minimum is
            // itself signed overflow. unsigned wrap-around is defined, and so is the conversion
            const uint64_t magnitude = 1ULL << (bits - 1);
            return static_cast<int64_t>(~magnitude + 1);
        }

        uint64_t get_max_positive_value() const {
            const unsigned bits = static_cast<unsigned>(size) * 8;
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

    std::string get_primitive_name(ValueTypePrimitive primitive);
    uint8_t get_primitive_size(ValueTypePrimitive primitive);
    char get_primitive_id_char(ValueTypePrimitive primitive);
    IntegerSize get_integer_size(ValueTypePrimitive primitive);

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

        static ValueType make_struct(ComplexType *complex_type, const std::vector<ValueType>& args = {}, TypeRegistry* registry = nullptr);

        static ValueType make_class(ComplexType *complex_type, const std::vector<ValueType>& args = {}, TypeRegistry* registry = nullptr);

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

        // `nullable` picks the spelling: true is `ptr<T>`, false the non-nullable borrow `T&`.
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

        ValueType() = default;
        ValueType(ValueTypePrimitive primitive) : 
            kind(ValueTypeKind::t_primitive), 
            primitive(primitive) 
        {}

        inline ComplexType *get_complex_type() const {
            assert(is_struct() || is_class());
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

        // true for `ptr<T>`, false for the borrow `T&`. only meaningful on a pointer
        bool is_nullable() const {
            assert(is_pointer());
            return type_flags & static_cast<uint8_t>(ValueTypeFlags::t_nullable);
        }

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

        bool is_numeric_type() const {
            if (!is_primitive()) {
                return false;
            }

            switch (this->primitive)
            {
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

            switch (this->primitive)
            {
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

            switch (this->primitive)
            {
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

            switch (this->primitive)
            {
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
        bool operator==(const ValueType& other) const {
            // First check if the kinds are different
            if (kind != other.kind) {
                return false;
            }

            // Check type flags (const, pointer, etc.)
            if (type_flags != other.type_flags) {
                return false;
            }

            // Compare based on the kind
            if (is_primitive() && other.is_primitive()) {
                return primitive == other.primitive;
            }

            // a pointer is structural: same nullability (already covered by the flag check
            // above) and the same pointee, all the way down
            if (is_pointer() && other.is_pointer()) {
                return *_pointee == *other._pointee;
            }

            if ((is_struct() || is_class()) && (other.is_struct() || other.is_class())) {
                // For struct and class types, compare the complex type pointers
                // Two struct/class types are equal if they point to the same ComplexType
                return _complex_type == other._complex_type;
            }

            if (is_type_param() && other.is_type_param()) {
                // identity is the declaration itself, mirroring how struct/class compare their
                // ComplexType. so the T of `struct Box<T>` is not the A of `struct Pair<A, B>`
                // even though both are the first parameter of their owner
                return _type_param == other._type_param;
            }

            if (kind == ValueTypeKind::t_unknown && other.kind == ValueTypeKind::t_unknown) {
                return true;
            }

            return false;
        }

        std::string get_mangled_name() const;

        std::string get_type_desciption() const;

    private:
        // defaulted so a default-constructed ValueType is a well-defined `unknown`
        // rather than carrying indeterminate kind/primitive.
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
        std::shared_ptr<const ValueType> _pointee = nullptr;

        ValueType(ValueTypeKind kind, ValueTypePrimitive primitive) : kind(kind), primitive(primitive) {}
        ValueType(ValueTypeKind kind, ComplexType *complex_type) :
            kind(kind),
            primitive(ValueTypePrimitive::t_complex),
            _complex_type(complex_type)
        {}
        ValueType(ValueTypeKind kind, const TypeParamDecl *param) :
            kind(kind), primitive(ValueTypePrimitive::t_void), _type_param(param) {}
    };
    
    class ComplexType {
    public:
        struct Property {
            size_t index;
            std::string name;
            ValueType type;
        };

        std::optional<std::string> name;

        // the namespace the type was declared in, null when unknown or root. instantiations
        // inherit it from their template, so a mangled name can always be fully qualified
        const Namespace *ast_namespace = nullptr;

        // this template's own generic parameters (the T, U in `struct Foo<T, U>`), owned by the
        // collector's TypeParamRegistry. empty for non-generics and for instantiations. always
        // append through add_type_parameter, which keeps the ordinal and owner consistent
        std::vector<TypeParamDecl *> type_parameters;
        ComplexType* template_ref = nullptr;  // Null for templates; points to original for instantiations
        std::vector<ValueType> instantiation_args;  // Empty for non-instantiated

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

        // appends a type parameter, stamping this type as its owner. asserts the ordinal matches
        // the position it lands in, so the ordinal can never drift from the list order.
        // defined out of line because it needs TypeParamDecl complete
        void add_type_parameter(TypeParamDecl *param);

        // true if `type` is a type parameter declared by this very type. the single place that
        // knows how a t_generic ValueType maps back to a declaration
        bool declares_type_param(const ValueType &type) const;

        void add_property(const std::string &name, ValueType type) {
            // on a template, a `T`-typed property must reference one of this type's own declared
            // parameters. instantiations carry no type_parameters of their own, so the check only
            // applies while a template is being built.
            if (type.is_type_param() && !is_instantiated()) {
                assert(declares_type_param(type));
            }
            _property_map[name] = _properties.size();
            _properties.push_back(Property { _properties.size(), name, type });
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

        // a concrete copy of this type: every property type run through `substitute`, and nothing
        // left that identifies it as a template or as somebody's instantiation.
        //
        // copy-then-modify rather than construct-then-refill, so a field added to ComplexType
        // survives by default and only the deliberate *drops* are named here. the previous shape -
        // a fresh ComplexType with the carried fields listed one by one - had already silently lost
        // the namespace once and the methods once, each time far from the cause
        template <typename Substitute>
        ComplexType substituted_copy(Substitute substitute) const
        {
            ComplexType copy(*this);

            // a copy is concrete: its parameter declarations would otherwise still point their
            // owner back at the template, and the instantiation identity would name the enclosing
            // instance's type arguments and mangle under its symbol
            copy.type_parameters.clear();
            copy.template_ref = nullptr;
            copy.instantiation_args.clear();

            // the property *names* are unchanged, so _property_map carries over as it is
            for (auto &prop : copy._properties) {
                prop.type = substitute(prop.type);
            }

            return copy;
        }

    private:
        std::vector<Property> _properties;
        std::unordered_map<std::string, size_t> _property_map;
        std::vector<FunctionDeclNode *> _methods;

        friend class TypeRegistry;  // Allow TypeRegistry to access _properties
    };

}  // namespace AST

// Hash support for ValueType to be used in unordered containers
namespace std {
    template<> struct hash<AST::ValueType> {
        size_t operator()(const AST::ValueType& vt) const {
            size_t h = static_cast<size_t>(vt.get_kind()) ^ vt.get_type_flags();
            if (vt.is_primitive()) h ^= static_cast<size_t>(vt.get_primitive_type());
            else if (vt.is_struct() || vt.is_class()) h ^= reinterpret_cast<size_t>(vt.get_complex_type());
            else if (vt.is_type_param()) h ^= reinterpret_cast<size_t>(vt.get_type_param());
            // mixed rather than xor'd: a bare xor of the pointee hash would make ptr<int32>
            // collide with int32, since the primitive component is identical
            else if (vt.is_pointer()) h ^= (*this)(vt.pointee()) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
}  // namespace std

namespace AST {

    // Hash support for tuple used in TypeRegistry
    struct TypeRegistryKeyHash {
        size_t operator()(const std::tuple<ComplexType*, std::vector<ValueType>>& key) const {
            size_t h1 = std::hash<ComplexType*>{}(std::get<0>(key));
            size_t h2 = 0;
            for (const auto& vt : std::get<1>(key)) {
                h2 ^= std::hash<AST::ValueType>{}(vt) + 0x9e3779b9 + (h2 << 6) + (h2 >> 2);
            }
            return h1 ^ (h2 << 1);
        }
    };

    // maps type-parameter declarations to the (possibly still-generic) types they stand for.
    // used identically for function and struct type parameters.
    //
    // keyed by declaration rather than by position, which is what makes a *partial* substitution
    // meaningful: substituting a generic member of a generic owner binds the owner's parameters
    // and leaves the member's own alone. composing two substitutions is concatenating bindings.
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
        static TypeSubstitution positional(const std::vector<TypeParamDecl *>& params, const std::vector<ValueType>& args) {
            assert(params.size() == args.size());

            TypeSubstitution subst;
            subst.bindings.reserve(params.size());
            for (size_t i = 0; i < params.size(); i++) {
                subst.bindings.emplace_back(params[i], args[i]);
            }
            return subst;
        }

        // rebinding an already bound parameter replaces it, so a later, better inference wins
        void bind(const TypeParamDecl *param, const ValueType& type) {
            assert(param);
            for (auto& binding : bindings) {
                if (binding.first == param) {
                    binding.second = type;
                    return;
                }
            }
            bindings.emplace_back(param, type);
        }

        // null when the parameter is not bound here, which substitute_type reads as "leave it"
        const ValueType *lookup(const TypeParamDecl *param) const {
            for (const auto& binding : bindings) {
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
    // pointer identity): equal argument lists always yield the same ComplexType*.
    class TypeRegistry {
    public:
        // register a template so that (template, {}) resolves back to the template itself.
        ComplexType* register_template(ComplexType* tmpl) {
            assert(tmpl->is_generic() && !tmpl->is_instantiated());
            auto key = std::make_tuple(tmpl, std::vector<ValueType>{});
            auto [it, inserted] = _instantiations.emplace(key, tmpl);
            return it->second;
        }

        // intern the instantiation of `tmpl` with `args`, substituting the template's properties
        // through `args`. Implemented in ASTValueType.cpp so it can call the unified substitute_type.
        ComplexType* get_or_create_instantiation(ComplexType* tmpl, const std::vector<ValueType>& args);

        // the interned concrete instantiations (Box<int>, ...), excluding the bare templates that
        // register_template maps to themselves. used by the --print-instances dump.
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
        std::string args_description(const std::vector<ValueType>& args) const;

        // owns the instantiation ComplexTypes this registry creates. Templates are owned elsewhere
        // (embedded on their StructDeclNode) and only appear here as map values, never in _owned.
        std::vector<std::unique_ptr<ComplexType>> _owned;
        std::unordered_map<std::tuple<ComplexType*, std::vector<ValueType>>, ComplexType*, TypeRegistryKeyHash> _instantiations;
    };

    // the single, unified type-substitution routine, shared by struct and function generics.
    // - a type-parameter reference resolves to subst[index], carrying its const/pointer flags;
    // - a generic application (a struct/class whose ComplexType is an instantiation) has its
    //   arguments recursively substituted and is then re-interned via `registry` - this is what
    //   makes nested generics such as Foo<Bar<T>> work;
    // - primitives and already-concrete types are returned unchanged.
    ValueType substitute_type(const ValueType& type, const TypeSubstitution& subst, TypeRegistry& registry);

    // true if the type is, or structurally contains, an unresolved type parameter - either directly
    // or as an argument of a generic application. after monomorphization a concrete context should be
    // free of these; anything left is a resolution bug rather than a legitimate type.
    bool contains_type_param(const ValueType& type);

    // true when nothing has answered what this type is yet: unknown, void, or still mentioning a
    // type parameter that a substitution has not bound.
    //
    // the single spelling of "no information", because every pass that reasons about types before
    // they are all known needs the same three-way distinction - a wrong type, a right type, and no
    // type yet. overload ranking reads it as neutral, generic inference as "cannot bind from this,
    // ask again later", and both used to say it in their own words
    inline bool is_undetermined_type(const ValueType &type) {
        return type.is_unknown() || type.is_void() || contains_type_param(type);
    }

    // the type a value-position read of `type` yields: the pointee for a pointer, the type itself
    // otherwise. exactly one level, never more - `ptr<ptr<uint8>>` reads as `ptr<uint8>`.
    //
    // one primitive behind two rules that are the same rule: the auto-deref that makes a pointer
    // behave like the value it points at, and the generic decay that binds T=int32 when a
    // ptr<int32> is passed to `function box<T>(T $v)`
    // (book/concept/pointers_and_refs_v2.md, "Pointers and generics")
    ValueType value_type_of(const ValueType &type);

    // the type ultimately addressed, following every pointer level rather than one.
    //
    // deliberately separate from value_type_of, which is the *read* rule and must stay at one
    // level. this is the `->` rule: a member lives on the struct, however many addresses deep
    // the base happens to be, so `ptr<ptr<Point>>` still finds Point's fields
    // (book/concept/pointers_and_refs_v2.md, "Structs and classes")
    ValueType target_type_of(const ValueType &type);

    // true when a value of `from` may be used where `to` is expected without an explicit cast.
    //
    // deliberately looser than operator==, which has to stay exact because it is the interning
    // identity for TypeRegistry and the monomorphizer's instance cache - Box<int32> and
    // Box<const int32> are different layouts even though an int32 argument satisfies a const
    // int32 parameter. the two differences here:
    //   - top level const is dropped
    //   - a non-nullable borrow T& widens to a nullable ptr<T>, never the reverse
    bool is_implicitly_convertible(const ValueType &from, const ValueType &to);

    // can this type decide what a number or bool literal is? only a concrete, non-void primitive
    // can. every other kind reaches a literal as a hint that says nothing about it:
    //   - a pointer hint belongs to the expression as a whole, not to the operand - in
    //     `ptr<int> $q = $p:$ + 1` the literal is only the element offset;
    //   - a type parameter says nothing until it is substituted;
    //   - a struct or class has no conversion from a literal at all.
    // in all of those the literal is typed on its own (int32 unless the value needs int64) and
    // fitted at its destination by TypeLowering::coerce_value after monomorphization, the same way
    // a `return` in a generic body is. treating them as hints instead made the autocast helpers
    // report a bogus "unexpected token" at the literal
    bool can_type_a_literal(const ValueType &type);

};

#endif