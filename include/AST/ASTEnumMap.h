#ifndef ASTENUMMAP_H
#define ASTENUMMAP_H

#pragma once

#include "AST/ASTEnumMapType.h"

#include <string>

namespace AST
{
    class Collector;
    class FunctionDeclNode;
    class Module;

    // **how a synthesized enum conversion lowers.** one question, so StmtCodegen does not re-derive
    // "is this glfw(), from(glfw:), or closed from()" from names and arity at the emit site.
    //
    // t_none is the ordinary body. the three LUT kinds share one emitter; the kind says which table
    // it indexes and whether a miss is possible
    enum class EnumLutKind
    {
        t_none,

        // `$key->glfw() : R`. total. `[n x key]` indexed by the case ordinal. R is an integer or
        // a payload-free enum; key is R itself, or R's discriminant
        t_forward,

        // `KeyCode::from(glfw: $raw) : KeyCode?`. partial. span compare, then a load
        t_reverse,

        // `HttpStatus::from($raw) : HttpStatus?`. the unnamed backing's reverse, same lowering
        t_closed_from,
    };

    // **the LUT identity of a synthesized declaration.** kind and the named map, one walk, so
    // codegen does not classify through template_ref and then search again by pointer equality.
    // `map` is set for t_forward / t_reverse; t_closed_from has none
    struct EnumLut
    {
        EnumLutKind kind = EnumLutKind::t_none;
        const EnumMap *map = nullptr;
    };

    // **may this be a map's range, given only the kind?** empty when it may. parse-time: pass 1
    // has recorded `enum` vs `struct`, but payload cases of a later file are not in yet. an
    // integer or an enum is a yes; mint asks enum_map_range_refusal once the case table is complete
    std::string enum_map_range_kind_refusal(const std::string &name, const ValueType &type);

    // **may this be a map's range, now that cases exist?** empty when it may. an integer, or a
    // payload-free non-generic enum. TypeChecker does not ask this - mint does, so a payload
    // target is refused before the functions are planted rather than as a second sentence later
    std::string enum_map_range_refusal(const ValueType &type);

    // **the integer a map's LUT indexes by.** the range itself, or an enum's `__tag`. asked of a
    // type enum_map_range_refusal left empty, so codegen does not re-derive which field of an
    // enum is the key
    ValueType enum_map_key_type(const ValueType &range);

    // asked of the declaration being emitted, never of a call. a user-written glfw() is t_none:
    // only is_implicitly_generated functions take the LUT path. identity is the stored
    // `forward` / `reverse` / `enum_closed_from` pointer, not the name. an instantiation
    // matches through `template_ref`
    EnumLut enum_lut_of(const FunctionDeclNode *decl);

    // foldable unique RHS, after ConstantExpander has rewritten each association. reports
    // into the collector and writes Association::folded_bits, which is what codegen reads.
    // TypeChecker asks once per module, walking File::enum_maps — File owns the record, the
    // owner enum may live in another module. walks the table, not the planted match, so a
    // user-written accessor does not silence the reverse's uniqueness check. a payload-free
    // case RHS (`.green`) folds through AST::fold_payload_free_enum_case, off the range and
    // the call's name, not off `decl`
    void check_enum_maps(Collector &collector, Module &module);
};

#endif
