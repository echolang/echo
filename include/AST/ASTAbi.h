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

    // why `#[export]` is not a table. empty when absent or nothing refuses it.
    // `exports_abi_tables` is CodegenTarget::exports_abi_tables()
    std::optional<std::string> abi_table_export_refusal(
        const TypeDeclNode &node,
        bool exports_abi_tables
    );

    // `#[export]` present and nothing refuses it
    bool type_exports_abi_table(const TypeDeclNode &node, bool exports_abi_tables);

    // leftover named payload, a row that has no ABI tables, a type that does
    // not answer an extern interface. TypeChecker asks once, next to
    // check_extern_conformances. duplicate C names stay on check_exports
    void check_abi_exports(Collector &collector, Bundle &bundle, bool exports_abi_tables);
};

#endif
