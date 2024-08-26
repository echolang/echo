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

    enum class ValueTypeKind {
        t_primitive,
        t_class,
        t_struct,
        t_generic,
        t_unknown
    };

    enum class ValueTypeFlags {
        t_const = 1 << 0,
        t_pointer = 1 << 1,
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
        t_float32,
        t_float64,
        t_bool,
        t_void,
    };

    struct IntegerSize {
        uint8_t size;
        bool is_signed;

        IntegerSize(uint8_t size, bool is_signed) : size(size), is_signed(is_signed) {}
        
        int64_t get_max_negative_value() const {
            if (!is_signed) return 0;
            return -(1LL << (size * 8 - 1));
        }

        uint64_t get_max_positive_value() const {
            if (is_signed)
                return (1ULL << (size * 8 - 1)) - 1;
            else
                return (1ULL << (size * 8)) - 1;
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

        static ValueType make_const(ValueType type) {
            type.set_const(true);
            return type;
        }

        static ValueType make_pointer(ValueType type) {
            type.set_pointer(true);
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
            return type_flags & static_cast<uint8_t>(ValueTypeFlags::t_pointer);
        }

        void set_const(bool is_const) {
            if (is_const) {
                type_flags |= static_cast<uint8_t>(ValueTypeFlags::t_const);
            } else {
                type_flags &= ~static_cast<uint8_t>(ValueTypeFlags::t_const);
            }
        }

        void set_pointer(bool is_pointer) {
            if (is_pointer) {
                type_flags |= static_cast<uint8_t>(ValueTypeFlags::t_pointer);
            } else {
                type_flags &= ~static_cast<uint8_t>(ValueTypeFlags::t_pointer);
            }
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
        
        inline ValueTypePrimitive get_primitive_type() const {
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
            _properties.push_back(Property { _properties.size(), name, type });
            _property_map[name] = type;
        }

        bool has_property(const std::string &name) const {
            return _property_map.find(name) != _property_map.end();
        }

        bool has_property(size_t index) const {
            return index < _properties.size();
        }

        const ValueType &get_property_type(const std::string &name) const {
            return _property_map.at(name);
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

    private:
        std::vector<Property> _properties;
        std::unordered_map<std::string, ValueType> _property_map;

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

};

#endif