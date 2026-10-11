#ifndef ASTABI_H
#define ASTABI_H

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace AST
{
    class Bundle;
    class Collector;
    class ComplexType;
    class TypeDeclNode;

    inline constexpr uint32_t k_abi_version = 1;

    // `__eco_abi_<mangled>` - the one spelling of a table symbol
    std::string abi_table_symbol(const ComplexType &ct);

    // fnv-1a 64 of requirement order, Echo names, C link names, and
    // C-erased signatures. pointee struct/enum/opaque names and layouts
    // fold recursively. uses Compiler::fnv1a64
    uint64_t abi_fingerprint(const ComplexType &ct);

    // `#[export]` on a type that answers an extern interface, with no
    // payload - the table is named from the interface. emit_tables and
    // check_exports both ask this, so a second walk cannot invent a
    // different set of table symbols
    bool type_exports_abi_table(const TypeDeclNode &node);

    // why this type's `#[export]` is not a table. nullopt when the
    // attribute is absent, or when type_exports_abi_table is true and
    // the target is not wasm. `wasm` is a bool, not an empty name list
    std::optional<std::string> abi_table_export_refusal(
        const TypeDeclNode &node,
        bool wasm
    );

    // leftover named payload, wasm, a type that does not answer an
    // extern interface. TypeChecker asks once, next to
    // check_extern_conformances. duplicate C names stay on check_exports
    void check_abi_exports(Collector &collector, Bundle &bundle, bool wasm);
};

#endif
