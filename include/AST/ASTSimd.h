#ifndef ASTSIMD_H
#define ASTSIMD_H

#pragma once

#include "AST/ASTBuiltin.h"
#include "AST/ASTValueType.h"

#include <optional>
#include <string>
#include <vector>

namespace AST
{
    // NEON, SSE2, and the heap allocator's alignment. a `bool` lane is one byte, so this
    // is also the widest `simd::bitmask`. the wide lowering static_asserts the value
    // stays 16: that path is a 16-lane split, not a general N. raising the cap is an
    // allocator question first
    constexpr uint64_t k_simd_max_bytes = 16;

    // **why may this `simd<T, N>` not exist?** three answers, so a not-yet does not read as a
    // refusal. asked once by TypeChecker of every type a declaration carries, beside void and
    // incomplete. reports nothing itself
    //
    //   t_fine     - T is a legal lane and N is a bound power of two whose width fits
    //   t_pending  - N is still a value parameter, or T is still a type parameter
    //   t_refused  - a sentence: wrong element, N not a power of two, or wider than 16 bytes
    //
    // walks wrappers (pointer, optional, T[N], a generic's arguments, a signature) so
    // `ptr<simd<string, 4>>` is the same sentence as `simd<string, 4>`
    enum class SimdShapeKind
    {
        t_fine,
        t_pending,
        t_refused,
    };

    struct SimdShape
    {
        SimdShapeKind kind = SimdShapeKind::t_fine;
        std::string sentence;
    };

    // the shape rule, one owner. `type` is the written type, which may wrap a vector
    SimdShape simd_shape_refusal(const ValueType &type);

    // **why may this type not cross a C boundary?** nullopt when it may. a vector's ABI
    // differs per platform, so a `simd` by value in an `extern function<...>` or an
    // `extern { }` signature is refused - including one nested in a struct, an inline
    // array, a tagged optional or a C function-pointer signature. a `ptr<simd<...>>` is
    // the spelling that may. one sentence, two readers
    std::optional<std::string> simd_crosses_c_refusal(const ValueType &type);

    // **is this one of the `simd::` verbs?** one owner so TypeChecker and the foldability
    // switches do not each keep a list that can drift
    bool is_simd_builtin(BuiltinKind kind);

    // **why may this verb not run over these bindings?** nullopt when it may.
    //
    // asked once, by AST::TypeChecker, of the builtin's bound `T` and `N` - rather than at
    // each of the several places a vector is named. the shape rule is simd_shape_refusal's;
    // this constructs the vector those bindings name and asks it. pending is silence
    std::optional<std::string> simd_operand_refusal(
        BuiltinKind kind,
        const std::vector<ValueType> &bindings);
};

#endif
