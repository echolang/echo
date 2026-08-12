#ifndef PRINTFCONVERSION_H
#define PRINTFCONVERSION_H

#pragma once

#include "AST/ASTNullability.h"
#include "AST/ASTValueType.h"

namespace Compiler::LLVM
{
    // how a primitive reaches printf: the conversion specifier, and the primitive the value has to be
    // widened to before it is handed to a variadic call. both halves sit in one row because C's default
    // argument promotions are part of the conversion rather than a step after it - printf reads exactly
    // what the format says, so `%d` against a raw i8 reads four bytes where one was passed, and `%f`
    // against a raw float reads a double. only the AArch64 backend performs those promotions on our
    // behalf, which is why passing the value raw looked correct on arm64 macOS and printed the
    // untruncated int (300 for an int8 holding 44) and 0.000000 for a float32 on x86-64
    //
    // the promoted type is chosen to agree with the format exactly - unsigned widens to uint32 under
    // `%u` rather than to the int32 C's integer promotion would strictly give - so the two cannot drift
    // apart. a table rather than an if-cascade so that adding a primitive is one row and cannot silently
    // reuse a neighbouring width: `usize`/`isize` print as their concrete 64 bit width, which is what
    // ECO_TARGET_POINTER_SIZE says on every target wired up today
    //
    // **its own header because two printers read it.** `echo` prints one scalar per statement and the
    // `dprint` builtin prints a whole value's structure, and a second copy of this table is the exact
    // shape of bug this file exists to prevent - one printer learning about a primitive the other still
    // reads at the wrong width. the row carries **no trailing newline**: where a line ends is the
    // caller's question, and only one of the two ends a line per value
    struct PrintfConversion
    {
        // null for a type that has no conversion
        const char *format;
        AST::ValueTypePrimitive promoted;
    };

    inline PrintfConversion printf_conversion_for(const AST::ValueType &type)
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
            case AST::ValueTypePrimitive::t_isize:
                return {"%lld", AST::ValueTypePrimitive::t_int64};

            case AST::ValueTypePrimitive::t_uint8:
            case AST::ValueTypePrimitive::t_uint16:
            case AST::ValueTypePrimitive::t_uint32:
                return {"%u", AST::ValueTypePrimitive::t_uint32};

            case AST::ValueTypePrimitive::t_uint64:
            case AST::ValueTypePrimitive::t_usize:
                return {"%llu", AST::ValueTypePrimitive::t_uint64};

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
