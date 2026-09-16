#include "AST/ASTEnumMap.h"

#include "AST/ASTCodeRef.h"
#include "AST/ASTCollector.h"
#include "AST/ASTConstFold.h"
#include "AST/ASTFile.h"
#include "AST/ASTModule.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "Token.h"

#include <fmt/core.h>

#include <cassert>
#include <map>

namespace
{
TokenReference map_token(const AST::EnumMap &map)
{
    if (map.map_span.is_valid()) {
        return map.map_span.first();
    }

    assert(map.name_span.is_valid());
    return map.name_span.first();
}

TokenReference association_token(
    const AST::EnumMap::Association &assoc,
    const AST::EnumMap &map
)
{
    if (assoc.case_span.is_valid()) {
        return assoc.case_span.first();
    }

    return map_token(map);
}

bool names_lut_decl(const AST::FunctionDeclNode *decl, const AST::FunctionDeclNode *stored)
{
    return stored != nullptr && (decl == stored || decl->template_ref == stored);
}

void check_one_map(
    const AST::ComplexType &owner,
    AST::EnumMap &map,
    AST::Collector &collector,
    AST::Module &module
)
{
    if (!map.minted) {
        return;
    }

    const AST::Module *origin = map.declared_in.module != nullptr
        ? map.declared_in.module
        : &module;

    auto at_ref = [&](const TokenReference &token) {
        return AST::CodeRef { origin, token.make_slice() };
    };

    std::map<uint64_t, size_t> seen;

    for (size_t i = 0; i < map.associations.size(); i++) {
        AST::EnumMap::Association &assoc = map.associations[i];
        const TokenReference at = association_token(assoc, map);

        AST::ExprNode *inner = AST::strip_implicit_casts(assoc.value);
        if (inner == nullptr) {
            collector.collect_issue<AST::Issue::GenericError>(
                at_ref(at),
                fmt::format(
                    "The '{}' of '{}' has to be a compile-time '{}'.",
                    map.name, owner.namespaced_name(), map.value_type.get_type_desciption()));
            continue;
        }

        AST::ConstFoldResult folded = AST::const_fold(inner);
        if (folded.result == AST::ConstFoldResult::Result::t_pending
            && inner->get_node_type() == AST::FunctionCallExprNode::node_type) {
            folded = AST::fold_payload_free_enum_case(
                *static_cast<AST::FunctionCallExprNode *>(inner), &map.value_type);
        }

        if (folded.is_folded()) {
            if (!(folded.type == map.value_type)) {
                collector.collect_issue<AST::Issue::GenericError>(
                    at_ref(at),
                    fmt::format(
                        "'.{}' maps to a '{}', and '{}' is declared as '{}'.",
                        assoc.case_name,
                        folded.type.get_type_desciption(),
                        map.name,
                        map.value_type.get_type_desciption()));
                continue;
            }

            auto existing = seen.find(folded.bits);
            if (existing != seen.end()) {
                const std::string &first = map.associations[existing->second].case_name;
                collector.collect_issue<AST::Issue::GenericError>(
                    at_ref(at),
                    fmt::format(
                        "'.{}' and '.{}' both map to the same '{}' - the reverse would not be a function.",
                        first, assoc.case_name, map.name));
                continue;
            }

            seen.emplace(folded.bits, i);
            assoc.folded_bits = folded.bits;
            continue;
        }

        if (folded.result == AST::ConstFoldResult::Result::t_pending) {
            collector.collect_issue<AST::Issue::GenericError>(
                at_ref(at),
                fmt::format(
                    "The '{}' of '{}' has to be a compile-time '{}' - this never settled.",
                    map.name, owner.namespaced_name(), map.value_type.get_type_desciption()));
            continue;
        }

        std::string why = folded.refusal.empty()
            ? fmt::format(
                "The '{}' of '{}' has to be a compile-time '{}'.",
                map.name, owner.namespaced_name(), map.value_type.get_type_desciption())
            : folded.refusal;

        collector.collect_issue<AST::Issue::GenericError>(
            at_ref(at), std::move(why));
    }
}
}

std::string AST::enum_map_range_kind_refusal(const std::string &name, const ValueType &type)
{
    if (type.is_integer_type() || type.is_enum()) {
        return "";
    }

    return fmt::format(
        "A map's type has to be an integer or an enum - '{}' is '{}'.",
        name, type.get_type_desciption());
}

std::string AST::enum_map_range_refusal(const ValueType &type)
{
    if (type.is_integer_type()) {
        return "";
    }

    if (!type.is_enum() || type.get_complex_type() == nullptr) {
        return fmt::format(
            "A map's type has to be an integer or an enum, and '{}' is not.",
            type.get_type_desciption());
    }

    const ComplexType *ct = type.get_complex_type();
    const std::string name = ct->namespaced_name();

    if (ct->is_generic()) {
        return fmt::format(
            "A map cannot target the generic enum '{}' - write it on a concrete enum.",
            name);
    }

    if (ct->has_payload_case()) {
        return fmt::format(
            "A map cannot target '{}' - a map is a relation over payload-free cases.",
            name);
    }

    return "";
}

AST::ValueType AST::enum_map_key_type(const ValueType &range)
{
    if (range.is_integer_type()) {
        return range;
    }

    assert(range.is_enum() && range.get_complex_type() != nullptr);
    return range.get_complex_type()->get_property_type(k_enum_tag_index);
}

AST::EnumLut AST::enum_lut_of(const FunctionDeclNode *decl)
{
    if (decl == nullptr || !decl->is_implicitly_generated || decl->owner_type == nullptr) {
        return {};
    }

    const ComplexType *ct = decl->owner_type->template_or_self();

    for (const EnumMap *map : ct->enum_maps()) {
        if (map == nullptr) {
            continue;
        }

        if (names_lut_decl(decl, map->forward)) {
            return { EnumLutKind::t_forward, map };
        }
        if (names_lut_decl(decl, map->reverse)) {
            return { EnumLutKind::t_reverse, map };
        }
    }

    if (names_lut_decl(decl, ct->enum_closed_from)) {
        return { EnumLutKind::t_closed_from, nullptr };
    }

    return {};
}

void AST::check_enum_maps(Collector &collector, Module &module)
{
    for (File &file : module.files()) {
        for (auto &map : file.enum_maps) {
            if (map == nullptr || map->owner == nullptr) {
                continue;
            }

            check_one_map(*map->owner, *map, collector, module);
        }
    }
}
