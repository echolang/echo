#include "AST/ASTValueType.h"
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