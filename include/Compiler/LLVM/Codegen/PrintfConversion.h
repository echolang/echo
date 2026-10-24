#ifndef PRINTFCONVERSION_H
#define PRINTFCONVERSION_H

#pragma once

#include "AST/ASTNullability.h"
#include "AST/ASTValueType.h"

namespace Compiler::LLVM
{
    // printf specifier and the primitive the value is widened to. C default promotions are part of
    // the conversion: `%d` against a raw i8 reads four bytes. `echo` and `dprint` share this table
    struct PrintfConversion
    {
        // null for a type that has no conversion
        const char *format;
        AST::ValueTypePrimitive promoted;
    };

    inline PrintfConversion printf_conversion_for(const AST::ValueType &type, unsigned pointer_bytes)
    {
        constexpr PrintfConversion unsupported{nullptr, AST::ValueTypePrimitive::t_void};

        // **a tagged optional prints its payload, and ignores whether it is there.** peeled through
        // AST::echo_printed_type_of rather than tested here, because AST::TypeChecker's echo arm decides
        // whether the program is *accepted* on the same question and the two must not be two conditions -
        // a program the checker admits and this table refuses is an internal error thrown at a user
        const AST::ValueType printed = AST::echo_printed_type_of(type);

        if (!printed.is_primitive()) {
            return unsupported;
        }

        switch (printed.get_primitive_type()) {
            case AST::ValueTypePrimitive::t_int8:
            case AST::ValueTypePrimitive::t_int16:
            case AST::ValueTypePrimitive::t_int32:
            case AST::ValueTypePrimitive::t_bool:
                return {"%d", AST::ValueTypePrimitive::t_int32};

            case AST::ValueTypePrimitive::t_int64:
                return {"%lld", AST::ValueTypePrimitive::t_int64};

            case AST::ValueTypePrimitive::t_isize:
                return pointer_bytes == 8
                    ? PrintfConversion{"%lld", AST::ValueTypePrimitive::t_int64}
                    : PrintfConversion{"%d", AST::ValueTypePrimitive::t_int32};

            case AST::ValueTypePrimitive::t_uint8:
            case AST::ValueTypePrimitive::t_uint16:
            case AST::ValueTypePrimitive::t_uint32:
                return {"%u", AST::ValueTypePrimitive::t_uint32};

            case AST::ValueTypePrimitive::t_uint64:
                return {"%llu", AST::ValueTypePrimitive::t_uint64};

            case AST::ValueTypePrimitive::t_usize:
                return pointer_bytes == 8
                    ? PrintfConversion{"%llu", AST::ValueTypePrimitive::t_uint64}
                    : PrintfConversion{"%u", AST::ValueTypePrimitive::t_uint32};

            // **`%f` is `echo`'s answer, not everyone's.** it renders 42.69 as `42.690000`, which is the
            // output the corpus has always pinned. `dprint` overrides both float rows with `%g` and
            // `%.15g`, which is a *presentation* choice its own renderer owns - the promotion is the part
            // that has to be shared, and it is the same either way
            case AST::ValueTypePrimitive::t_float32:
            case AST::ValueTypePrimitive::t_float64:
                return {"%f", AST::ValueTypePrimitive::t_float64};

            default:
                return unsupported;
        }
    }
};

#endif
