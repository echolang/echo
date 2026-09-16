#ifndef ASTENUMMAPTYPE_H
#define ASTENUMMAPTYPE_H

#pragma once

#include "AST/ASTDeclarationOrigin.h"
#include "AST/ASTValueType.h"
#include "AST/ASTVisibility.h"
#include "Token.h"

#include <optional>
#include <string>
#include <vector>

namespace AST
{
    class ComplexType;
    class ExprNode;
    class FunctionDeclNode;
    class Namespace;

    // **a named compile-time relation from every case to an integer or a payload-free enum**,
    // written `map glfw : int32` / `map color : Color` in the enum body, or with `for KeyCode`
    // at file scope when the enum lives elsewhere. not a match, not a constructor, not the
    // unnamed backing: it mints `$key->glfw() : int32` (total) and
    // `KeyCode::from(glfw: $raw) : KeyCode?` (partial). an enum range is the same relation
    // keyed on the target's discriminant.
    //
    // the file that wrote it owns the record (`File::enum_maps`). ComplexType holds pointers
    // for name lookup and LUT identity. ASTEnumMap.h is the owner of the questions asked of it
    struct EnumMap
    {
        std::string name;
        TokenSpan name_span;
        TokenSpan map_span;
        ValueType value_type;

        // the enum this relation is on. a file-scope map may name an enum in another module
        ComplexType *owner = nullptr;

        // where this map was written, and who may name the methods it mints. an in-body map
        // is the enum's file and `t_public` (as reachable as the enum). a file-scope map is
        // a top-level declaration of *that* module, so the default is the module and
        // `public` is what a GLFW binding writes
        DeclarationOrigin declared_in;
        Visibility visibility = Visibility::t_public;
        Namespace *declared_namespace = nullptr;

        struct Association
        {
            std::string case_name;
            TokenSpan case_span;
            size_t case_ordinal = 0;
            TokenSpan value_span;

            // the written RHS. the forward match clones it (one node, one parent). this
            // pointer is what check_enum_maps folds, including when the author already
            // wrote glfw() and no match was planted
            ExprNode *value = nullptr;

            // set by check_enum_maps once the RHS folded. codegen reads this rather than
            // walking the planted match
            std::optional<uint64_t> folded_bits;
        };

        std::vector<Association> associations;

        // the two functions minted from this relation. null when the author already wrote
        // one of that name, matching today's `from` skip, or before mint_file_enum_maps
        FunctionDeclNode *forward = nullptr;
        FunctionDeclNode *reverse = nullptr;

        // every source case appears. the forward returns R rather than R?. omitted cases
        // are the holes: `$error->color()` is null, and listing every case is what keeps
        // the accessor total
        bool total = true;

        // mint_one_map finished: cases exist, the range is legal. uniqueness still runs
        // when the author already wrote glfw() and forward stayed null
        bool minted = false;
    };
};

#endif
