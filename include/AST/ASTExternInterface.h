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

    // requirements in declaration order. look a slot up by name, not layout index
    std::vector<FunctionDeclNode *> extern_interface_requirements(const ComplexType &ct);

    // no zero value: a null slot jumps on the first call
    std::optional<std::string> extern_interface_zero_refusal(const class ValueType &type);

    // a local with no initializer and no unwrapped binding
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
