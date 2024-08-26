#include "AST/ASTValueType.h"

#include "AST/ASTNamespace.h"
#include "AST/ASTTypeParam.h"
#include "External/infint.h"

#include <cassert>

std::string AST::get_primitive_name(ValueTypePrimitive primitive)
{
    switch (primitive) {
        case ValueTypePrimitive::t_complex: return "complex";
        case ValueTypePrimitive::t_int8: return "int8";
        case ValueTypePrimitive::t_int16: return "int16";
        case ValueTypePrimitive::t_int32: return "int32";
        case ValueTypePrimitive::t_int64: return "int64";
        case ValueTypePrimitive::t_uint8: return "uint8";
        case ValueTypePrimitive::t_uint16: return "uint16";
        case ValueTypePrimitive::t_uint32: return "uint32";
        case ValueTypePrimitive::t_uint64: return "uint64";
        case ValueTypePrimitive::t_float32: return "float32";
        case ValueTypePrimitive::t_float64: return "float64";
        case ValueTypePrimitive::t_bool: return "bool";
        case ValueTypePrimitive::t_void: return "void";

        default: return "";
    };
}

uint8_t AST::get_primitive_size(ValueTypePrimitive primitive)
{
    switch (primitive) {
        case ValueTypePrimitive::t_int8: return 1;
        case ValueTypePrimitive::t_int16: return 2;
        case ValueTypePrimitive::t_int32: return 4;
        case ValueTypePrimitive::t_int64: return 8;
        case ValueTypePrimitive::t_uint8: return 1;
        case ValueTypePrimitive::t_uint16: return 2;
        case ValueTypePrimitive::t_uint32: return 4;
        case ValueTypePrimitive::t_uint64: return 8;
        case ValueTypePrimitive::t_float32: return 4;
        case ValueTypePrimitive::t_float64: return 8;
        case ValueTypePrimitive::t_bool: return 1;

        default: return 0;
    };
}

char AST::get_primitive_id_char(ValueTypePrimitive primitive)
{
    switch (primitive) {
        case ValueTypePrimitive::t_int8: return 'c';
        case ValueTypePrimitive::t_int16: return 's';
        case ValueTypePrimitive::t_int32: return 'i';
        case ValueTypePrimitive::t_int64: return 'l';
        case ValueTypePrimitive::t_uint8: return 'C';
        case ValueTypePrimitive::t_uint16: return 'S';
        case ValueTypePrimitive::t_uint32: return 'I';
        case ValueTypePrimitive::t_uint64: return 'L';
        case ValueTypePrimitive::t_float32: return 'f';
        case ValueTypePrimitive::t_float64: return 'd';
        case ValueTypePrimitive::t_bool: return 'b';
        case ValueTypePrimitive::t_void: return 'v';

        default: return 'u';
    };
}

AST::IntegerSize AST::get_integer_size(ValueTypePrimitive primitive)
{
    const uint8_t size = get_primitive_size(primitive);

    switch (primitive) {
        case ValueTypePrimitive::t_int8: return AST::IntegerSize(size, true);
        case ValueTypePrimitive::t_int16: return AST::IntegerSize(size, true);
        case ValueTypePrimitive::t_int32: return AST::IntegerSize(size, true);
        case ValueTypePrimitive::t_int64: return AST::IntegerSize(size, true);
        case ValueTypePrimitive::t_uint8: return AST::IntegerSize(size, false);
        case ValueTypePrimitive::t_uint16: return AST::IntegerSize(size, false);
        case ValueTypePrimitive::t_uint32: return AST::IntegerSize(size, false);
        case ValueTypePrimitive::t_uint64: return AST::IntegerSize(size, false);

        default: 
            assert(false && "Invalid integer type");
            return AST::IntegerSize(0, false);
    };
}

// Implementation of new static factory methods
AST::ValueType AST::ValueType::make_struct(ComplexType *complex_type, const std::vector<ValueType>& args, TypeRegistry* registry) {
    if (!args.empty()) {
        assert(complex_type->is_generic());
        if (registry) {
            complex_type = registry->get_or_create_instantiation(complex_type, args);
        }
    }
    return ValueType(ValueTypeKind::t_struct, complex_type);
}

AST::ValueType AST::ValueType::make_class(ComplexType *complex_type, const std::vector<ValueType>& args, TypeRegistry* registry) {
    if (!args.empty()) {
        assert(complex_type->is_generic());
        if (registry) {
            complex_type = registry->get_or_create_instantiation(complex_type, args);
        }
    }
    return ValueType(ValueTypeKind::t_class, complex_type);
}

bool AST::ValueType::will_fit_into(ValueType other) const
{
    if (!(is_primitive() && other.is_primitive())) {
        return false;
    }

    // for floating types we can just check if the size is smaller
    if (is_floating_type() && other.is_floating_type()) {
        return get_primitive_size(primitive) <= get_primitive_size(other.primitive);
    }            

    // for integers we need to check if the size is smaller and if the sign is compatible
    else if (is_numeric_type() && other.is_numeric_type()) {
        if (is_signed_integer() && !other.is_signed_integer()) {
            return false;
        }

        return get_primitive_size(primitive) <= get_primitive_size(other.primitive);
    }

    // bool will fit into all numeric types
    else if (primitive == ValueTypePrimitive::t_bool) {
        return other.is_numeric_type();
    }

    return false;
}

bool AST::ValueType::is_same_size(ValueType other) const
{
    if (!(is_primitive() && other.is_primitive())) {
        return false;
    }

    return get_primitive_size(primitive) == get_primitive_size(other.primitive);
}

std::string AST::ComplexType::namespaced_name() const
{
    std::string type_name = name.value_or("[anonymous]");

    if (ast_namespace) {
        std::string ns = ast_namespace->full_name();
        if (!ns.empty()) {
            return ns + ECO_NAMESPACE_SEPARATOR + type_name;
        }
    }

    return type_name;
}

// length prefixed token, so concatenated parts can never be read back ambiguously
// ("a" + "Foo" and "aFoo" both become distinct tokens)
static std::string mangle_length_prefixed(const std::string &part)
{
    return std::to_string(part.size()) + part;
}

std::string AST::ComplexType::mangled_token() const
{
    // an instantiation's own name is the display string ("Box<int32>"), so the token is built
    // from the template plus the recursively mangled arguments instead
    if (is_instantiated() && template_ref) {
        std::string token = template_ref->mangled_token() + "I";
        for (const auto &arg : instantiation_args) {
            token += arg.get_mangled_name();
        }
        return token + "E";
    }

    if (!name.has_value()) {
        return "0";
    }

    std::vector<std::string> segments = ast_namespace ? ast_namespace->path_segments() : std::vector<std::string>{};

    // unqualified types stay a plain token, the `N...E` wrapper only appears when there
    // actually is a namespace path to separate from the name
    if (segments.empty()) {
        return mangle_length_prefixed(name.value());
    }

    std::string token = "N";
    for (const auto &segment : segments) {
        token += mangle_length_prefixed(segment);
    }
    token += mangle_length_prefixed(name.value());

    return token + "E";
}

std::string AST::ValueType::get_mangled_name() const
{
    std::string mangled_name = "";

    // const or mutable
    if (is_const()) {
        mangled_name += "C"; // const
    } else {
        mangled_name += "M"; // mutable
    }

    // pointer or lvalue
    if (is_pointer()) {
        mangled_name += "R"; // rvalue
    } else {
        mangled_name += "L"; // lvalue
    }

    // primitive type
    if (is_primitive()) {
        mangled_name += "P"; // primitive type
        mangled_name += get_primitive_id_char(primitive);
    } else if (is_type_param()) {
        // the ordinal, not the declaration's address: this feeds decorated_func_name and so the
        // LLVM symbol table, which has to stay reproducible across runs. only a template mangles
        // a type parameter at all, and a template is never emitted
        mangled_name += "T"; // type parameter
        mangled_name += std::to_string(_type_param->ordinal);
    } else if (is_struct() || is_class()) {
        mangled_name += "C"; // complex type
        mangled_name += get_complex_type()->mangled_token();
    } else {
        mangled_name += "U"; // unknown type
        mangled_name += "A";
    }

    return mangled_name;
}

std::string AST::ValueType::get_type_desciption() const
{
    std::string prefix = is_const() ? "const " : "";
    std::string pointer = is_pointer() ? "*" : "";

    if (is_primitive()) {
        return prefix + get_primitive_name(primitive) + pointer;
    }

    // the name the user wrote, unqualified: this feeds the interned name of every generic
    // application, so qualifying it here would render Box<int> as Box<Box::T> in the template.
    // TypeParamDecl::describe() is the qualified form, for diagnostics
    if (is_type_param()) {
        return prefix + _type_param->name + pointer;
    }

    if (is_struct() || is_class()) {
        ComplexType* ct = get_complex_type();
        if (!ct->name.has_value()) {
            return prefix + "[unknown]" + pointer;
        }

        // If this is an instantiated generic type, the name already includes template args
        // from the TypeRegistry's args_description method. the namespace is prepended here so
        // two same-named types from different namespaces are distinguishable in diagnostics
        return prefix + ct->namespaced_name() + pointer;
    }

    // Handle unknown or other types
    return prefix + "[unknown]" + pointer;
}

AST::ComplexType* AST::TypeRegistry::get_or_create_instantiation(ComplexType* tmpl, const std::vector<ValueType>& args)
{
    assert(tmpl->is_generic() && tmpl->type_parameters.size() == args.size());

    auto key = std::make_tuple(tmpl, args);
    if (auto it = _instantiations.find(key); it != _instantiations.end()) {
        ComplexType* inst = it->second;
        // refresh if the template gained properties after this instance was interned - this
        // happens when an application (e.g. a return type) is parsed during the symbol pass,
        // before the struct body populates the template's properties.
        if (inst != tmpl && inst->property_count() < tmpl->property_count()) {
            TypeSubstitution subst = TypeSubstitution::positional(tmpl->type_parameters, args);
            inst->_properties.clear();
            inst->_property_map.clear();
            for (const auto& prop : tmpl->_properties) {
                inst->add_property(prop.name, substitute_type(prop.type, subst, *this));
            }
        }
        return inst;
    }

    auto owned = std::make_unique<ComplexType>();
    ComplexType* instantiated = owned.get();
    _owned.push_back(std::move(owned));

    if (tmpl->name) {
        instantiated->name = tmpl->name.value() + "<" + args_description(args) + ">";
    }
    instantiated->ast_namespace = tmpl->ast_namespace;
    instantiated->template_ref = tmpl;
    instantiated->instantiation_args = args;

    // insert into the cache BEFORE substituting properties, so a self-referential generic
    // (e.g. a property of type ptr<Self<T>>) resolves back to this in-progress instance
    // instead of recursing forever.
    _instantiations[key] = instantiated;

    // one substitution for the whole layout: the arguments arrive positionally, matching the
    // template's declared parameter order, and positional() is where that arity is checked
    TypeSubstitution subst = TypeSubstitution::positional(tmpl->type_parameters, args);
    for (const auto& prop : tmpl->_properties) {
        instantiated->add_property(prop.name, substitute_type(prop.type, subst, *this));
    }

    return instantiated;
}

std::string AST::TypeRegistry::args_description(const std::vector<ValueType>& args) const
{
    std::string desc;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) desc += ",";
        desc += args[i].get_type_desciption();
    }
    return desc;
}

void AST::ComplexType::add_type_parameter(TypeParamDecl *param)
{
    assert(param);
    assert(param->ordinal == type_parameters.size());
    param->set_owner(this);
    type_parameters.push_back(param);
}

bool AST::ComplexType::declares_type_param(const ValueType& type) const
{
    assert(type.is_type_param());

    // pointer membership, which is a stronger check than the bounds test it replaces: a parameter
    // of some *other* generic no longer passes just because the ordinals happen to line up
    const TypeParamDecl *param = type.get_type_param();
    for (const auto *declared : type_parameters) {
        if (declared == param) {
            return true;
        }
    }
    return false;
}

bool AST::contains_type_param(const ValueType& type)
{
    if (type.is_type_param()) {
        return true;
    }

    // a generic application is unresolved if any of its arguments still is
    if (type.is_struct() || type.is_class()) {
        ComplexType* ct = type.get_complex_type();
        if (ct && ct->is_instantiated()) {
            for (const auto& arg : ct->instantiation_args) {
                if (contains_type_param(arg)) {
                    return true;
                }
            }
        }
    }

    return false;
}

AST::ValueType AST::substitute_type(const ValueType& type, const TypeSubstitution& subst, TypeRegistry& registry)
{
    // a type-parameter reference resolves to its bound type, carrying the reference's flags.
    // an *unbound* parameter is returned unchanged rather than asserting: that is what makes a
    // partial substitution well defined, so a generic member of a generic owner can have the
    // owner's parameters resolved while its own stay generic. the arity check that used to live
    // here now sits in TypeSubstitution::positional, which knows what full coverage means
    if (type.is_type_param()) {
        const ValueType *bound = subst.lookup(type.get_type_param());
        if (!bound) {
            return type;
        }

        ValueType resolved = *bound;
        if (type.is_const()) resolved.set_const(true);
        if (type.is_pointer()) resolved.set_pointer(true);
        return resolved;
    }

    // a generic application: recursively substitute its arguments, then re-intern.
    if (type.is_struct() || type.is_class()) {
        ComplexType* ct = type.get_complex_type();
        if (ct && ct->is_instantiated()) {
            std::vector<ValueType> resolved_args;
            resolved_args.reserve(ct->instantiation_args.size());
            for (const auto& arg : ct->instantiation_args) {
                resolved_args.push_back(substitute_type(arg, subst, registry));
            }
            ComplexType* inst = registry.get_or_create_instantiation(ct->template_ref, resolved_args);
            ValueType result = type.is_struct() ? ValueType::make_struct(inst) : ValueType::make_class(inst);
            if (type.is_const()) result.set_const(true);
            if (type.is_pointer()) result.set_pointer(true);
            return result;
        }
    }

    // primitives and already-concrete types are unchanged.
    return type;
}