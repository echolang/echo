#ifndef ASTEXTERNINTERFACE_H
#define ASTEXTERNINTERFACE_H

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace AST
{
    class Bundle;
    class Collector;
    class ComplexType;
    class FunctionDeclNode;

    // `__s<ordinal>_<name>` - the one spelling of an extern-interface slot
    std::string extern_slot_property_name(size_t ordinal, const std::string &requirement_name);

    // `$__image` - the erased library handle that keeps the image loaded
    inline constexpr const char *k_extern_image_property = "__image";

    // the requirements an ABI table, fingerprint, and slot_symbol walk, in
    // declaration order. nulls in methods() are skipped here, once
    std::vector<FunctionDeclNode *> extern_interface_requirements(const ComplexType &ct);

    // TypeChecker: an uninitialized non-nullable extern-interface local would
    // jump through null slots. empty when the declaration is fine
    std::optional<std::string> extern_interface_uninitialized_refusal(const class VarDeclNode &decl);

    // the static on `conformer` that answers `requirement`, or a sentence.
    // one owner: check_extern_conformances and the table lowering both ask
    struct ExternConformanceAnswer
    {
        FunctionDeclNode *decl = nullptr;
        std::optional<std::string> refusal;
    };

    ExternConformanceAnswer extern_conformance_answer(
        const ComplexType &conformer,
        const FunctionDeclNode &requirement
    );

    void check_extern_interface_declarations(Collector &collector, Bundle &bundle);

    // each extern conformance: a matching non-generic static per requirement
    void check_extern_conformances(Collector &collector, Bundle &bundle);
};

#endif
