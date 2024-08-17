#include "AST/ASTValueType.h"

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
        mangled_name += "T"; // type parameter
        mangled_name += std::to_string(type_param_index);
    } else if (is_struct() || is_class()) {
        mangled_name += "C"; // complex type @TODO
        mangled_name += get_complex_type()->name.value_or("A");
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

    if (is_type_param()) {
        return prefix + "T" + std::to_string(type_param_index) + pointer;
    }

    if (is_struct() || is_class()) {
        ComplexType* ct = get_complex_type();
        std::string type_name = ct->name.value_or("[unknown]");

        // If this is an instantiated generic type, the name already includes template args
        // from the TypeRegistry's args_description method
        return prefix + type_name + pointer;
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
        // refresh if the template gained properties after this instance was interned — this
        // happens when an application (e.g. a return type) is parsed during the symbol pass,
        // before the struct body populates the template's properties.
        if (inst != tmpl && inst->property_count() < tmpl->property_count()) {
            inst->_properties.clear();
            inst->_property_map.clear();
            for (const auto& prop : tmpl->_properties) {
                inst->add_property(prop.name, substitute_type(prop.type, args, *this));
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
    instantiated->template_ref = tmpl;
    instantiated->instantiation_args = args;

    // insert into the cache BEFORE substituting properties, so a self-referential generic
    // (e.g. a property of type ptr<Self<T>>) resolves back to this in-progress instance
    // instead of recursing forever.
    _instantiations[key] = instantiated;

    for (const auto& prop : tmpl->_properties) {
        ValueType subst = substitute_type(prop.type, args, *this);
        instantiated->add_property(prop.name, subst);
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

AST::ValueType AST::substitute_type(const ValueType& type, const TypeSubstitution& subst, TypeRegistry& registry)
{
    // a type-parameter reference resolves to its bound type, carrying the reference's flags.
    if (type.is_type_param()) {
        size_t idx = type.get_type_param_index();
        assert(idx < subst.size());
        ValueType resolved = subst[idx];
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