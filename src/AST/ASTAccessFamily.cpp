#include "AST/ASTAccessFamily.h"

AST::AccessFamily AST::access_family_of(const AST::ValueType &type)
{
    // an address is one family whatever it addresses - see the header
    if (type.is_pointer() || type.is_class() || type.is_weak() || type.is_callable() || type.is_interface()) {
        return AST::AccessFamily::t_address;
    }

    if (!type.is_primitive() || type.is_void()) {
        return AST::AccessFamily::t_none;
    }

    switch (AST::ValueType::make_mutable(type).get_primitive_type()) {
    // byte access, and the ancestor rather than a leaf: reading an object's bytes is a thing this
    // language spells, so these have to alias everything
    case AST::ValueTypePrimitive::t_int8:
    case AST::ValueTypePrimitive::t_uint8:
        return AST::AccessFamily::t_byte;

    // a signed and an unsigned reading of one width are one family. two spellings of one bit
    // pattern, and a promotion between them is something an author may legitimately promise
    case AST::ValueTypePrimitive::t_int16:
    case AST::ValueTypePrimitive::t_uint16:
        return AST::AccessFamily::t_integer16;

    case AST::ValueTypePrimitive::t_int32:
    case AST::ValueTypePrimitive::t_uint32:
        return AST::AccessFamily::t_integer32;

    case AST::ValueTypePrimitive::t_int64:
    case AST::ValueTypePrimitive::t_uint64:
        return AST::AccessFamily::t_integer64;

    // **by width, not by name.** `usize` and `uint64` on a 64-bit target are one family rather than
    // two that merely happen to be the same size - otherwise a promotion between them, which the
    // type system allows, would carry a separation the language never claimed
    case AST::ValueTypePrimitive::t_usize:
    case AST::ValueTypePrimitive::t_isize:
        return AST::get_primitive_size(AST::ValueTypePrimitive::t_usize) == 8
            ? AST::AccessFamily::t_integer64
            : AST::AccessFamily::t_integer32;

    case AST::ValueTypePrimitive::t_float32:
        return AST::AccessFamily::t_float32;

    case AST::ValueTypePrimitive::t_float64:
        return AST::AccessFamily::t_float64;

    // one byte of storage holding 0 or 1. it shares the byte family rather than getting one of its
    // own, because `uint8&` over a `bool` is the ordinary way to look at one
    case AST::ValueTypePrimitive::t_bool:
        return AST::AccessFamily::t_byte;

    case AST::ValueTypePrimitive::t_complex:
    case AST::ValueTypePrimitive::t_void:
        return AST::AccessFamily::t_none;
    }

    return AST::AccessFamily::t_none;
}

bool AST::access_families_may_alias(AST::AccessFamily a, AST::AccessFamily b)
{
    // nothing claimed, so nothing separated
    if (a == AST::AccessFamily::t_none || b == AST::AccessFamily::t_none) {
        return true;
    }

    // the ancestor reaches every descendant, from either end
    if (a == AST::AccessFamily::t_byte || b == AST::AccessFamily::t_byte) {
        return true;
    }

    return a == b;
}

const char *AST::access_family_name(AST::AccessFamily family)
{
    switch (family) {
    case AST::AccessFamily::t_none:
        return "";
    case AST::AccessFamily::t_byte:
        return "byte";
    case AST::AccessFamily::t_integer16:
        return "integer16";
    case AST::AccessFamily::t_integer32:
        return "integer32";
    case AST::AccessFamily::t_integer64:
        return "integer64";
    case AST::AccessFamily::t_float32:
        return "float32";
    case AST::AccessFamily::t_float64:
        return "float64";
    case AST::AccessFamily::t_address:
        return "address";
    }

    return "";
}
