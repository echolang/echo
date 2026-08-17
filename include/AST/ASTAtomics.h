#ifndef ASTATOMICS_H
#define ASTATOMICS_H

#pragma once

#include "AST/ASTBuiltin.h"
#include "AST/ASTValueType.h"

#include <optional>
#include <string>

namespace AST
{
    // **is this one of the seven `mem::atomic::` verbs?** one owner so TypeChecker and the
    // foldability switches do not each keep a list that can drift
    bool is_atomic_builtin(BuiltinKind kind);

    // **why may this type not sit under this atomic verb?** nullopt when it may.
    //
    // asked once, by AST::TypeChecker, of the builtin's bound `T` - rather than at each of the
    // several places a slot is named. one sweep is what keeps `atomic<string>::load` and a free
    // `mem::atomic::load<string>` refused by the same sentence
    //
    // admits the 8/16/32/64-bit integers signed and unsigned, `usize`/`isize`, `bool` (except
    // add/sub), and `ptr<T>` / a C function pointer at load/store/exchange/compare_exchange.
    // four refusals, and they are the content:
    //
    //   **float**                    - `atomicrmw fadd` would not mean what integer `add` means
    //   **`ptr<T>` at `add`/`sub`**  - moves the address by bytes, not elements
    //   **a class, `weak<T>`, an interface** - exchange moves the bits without moving the count
    //   **a struct, an enum**        - wider than a word, and an enum's `__tag` is the compiler's
    //
    // `bool` at `add`/`sub` is the fifth: there is no integer RMW on a flag. LLVM cannot emit an
    // atomic `i1` add either, so the refusal is also what keeps the lowering from lying
    std::optional<std::string> atomic_operand_refusal(
        const ValueType &type,
        BuiltinKind kind);
};

#endif
