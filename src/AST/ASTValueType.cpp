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
    } else {
        mangled_name += "C"; // complex type
        assert(false && "Not implemented");
    }

    return mangled_name;
}