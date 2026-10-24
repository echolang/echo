#include "AST/ASTMapLiteral.h"

#include "AST/ASTArgumentFit.h"
#include "AST/ASTCollector.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTInstantiation.h"
#include "AST/ASTTypeParam.h"
#include "AST/ExprNode.h"

#include <fmt/core.h>

namespace
{
    AST::MapLiteralLookup refuse(std::string reason)
    {
        AST::MapLiteralLookup lookup;
        lookup.result = AST::MapLiteralLookup::Result::t_refused;
        lookup.refusal = std::move(reason);
        return lookup;
    }
}

AST::MapLiteralExprNode *AST::map_literal_of(AST::ExprNode *expr)
{
    if (expr == nullptr || expr->get_node_type() != NodeType::n_expr_map_literal) {
        return nullptr;
    }

    return static_cast<MapLiteralExprNode *>(expr);
}

bool AST::bind_map_literal_to(
    AST::ExprNode *expr,
    const AST::ValueType &destination,
    AST::Collector &collector
)
{
    AST::MapLiteralExprNode *literal = map_literal_of(expr);

    if (literal == nullptr) {
        return false;
    }

    const AST::ValueType wanted = AST::implicit_conversion_target(destination);

    if (is_undetermined_type(wanted)) {
        return false;
    }

    const CollectionFill fill = collection_fill_of(wanted, collector, *literal);

    if (fill.kind != CollectionFill::Kind::t_keyed) {
        return false;
    }

    if (!literal->bound_type.has_value()) {
        literal->bound_type = wanted;
    }

    for (MapLiteralExprNode::Entry &entry : literal->entries) {
        if (!is_undetermined_type(fill.key)) {
            bind_collection_literal_to(entry.key, fill.key, collector);
        }

        if (!is_undetermined_type(fill.value)) {
            bind_collection_literal_to(entry.value, fill.value, collector);
        }
    }

    return true;
}

AST::MapLiteralLookup AST::map_literal_type_for(
    const AST::MapLiteralExprNode &literal,
    const AST::CoreTypes &core,
    AST::TypeRegistry &types
)
{
    AST::ComplexType *map_tmpl = core.declared_template(AST::CoreTypeKind::t_map);

    if (map_tmpl == nullptr) {
        return refuse(
            "a map literal with no declared type needs the core map type, and nothing in this "
            "program declares '#[echoc::core: map]'. it lives in the standard library, which this "
            "compilation left out - write the type, e.g. 'map<string, int32> $m = [...];'.");
    }

    if (!map_tmpl->is_generic() || map_tmpl->type_parameters.size() != 2) {
        return refuse(fmt::format(
            "'{}' is declared '#[echoc::core: map]' but does not take a key type and a value type, "
            "so a map literal cannot be built from it.",
            map_tmpl->name.value_or("the core map type")));
    }

    if (literal.entries.empty()) {
        return refuse(fmt::format(
            "an empty map literal has nothing to go on - it takes its type from where it is going. "
            "Write the type, e.g. '{}<string, int32> $m = [];'.",
            core.spelling(AST::CoreTypeKind::t_map)));
    }

    // **the first entry decides, and the rest are fitted to it.** not a common type over all of
    // them: the expansion writes one `$dest[key] = value` per entry, so AST::CallResolver already
    // judges every other pair against this one
    //
    // a nested collection literal has no result_type of its own, so `$m = ['a' => [1, 2]]` asks
    // collection_entry_type_for of the inner list rather than waiting on a type that expansion
    // never writes
    const AST::MapLiteralLookup key_look =
        collection_entry_type_for(literal.entries.front().key, core, types);

    if (key_look.result != AST::MapLiteralLookup::Result::t_ok) {
        return key_look;
    }

    const AST::MapLiteralLookup value_look =
        collection_entry_type_for(literal.entries.front().value, core, types);

    if (value_look.result != AST::MapLiteralLookup::Result::t_ok) {
        return value_look;
    }

    const AST::ValueType key = key_look.type;
    const AST::ValueType value = value_look.type;

    const std::vector<AST::ValueType> args {
        AST::ValueType::make_mutable(key),
        AST::ValueType::make_mutable(value),
    };

    const AST::ConstraintJudgement judged =
        AST::first_constraint_violation(map_tmpl->type_parameters, args, types);

    if (judged.violation.has_value()) {
        const auto *param = map_tmpl->type_parameters[*judged.violation];

        return refuse(fmt::format(
            "'{}' cannot hold a '{}' - its {} type is constrained to '{}'.",
            map_tmpl->name.value_or("the core map type"),
            args[*judged.violation].get_type_desciption(),
            *judged.violation == 0 ? "key" : "value",
            param->constraint_spelling));
    }

    AST::MapLiteralLookup lookup;
    lookup.result = AST::MapLiteralLookup::Result::t_ok;
    lookup.type = AST::ValueType::make_complex(types.get_or_create_instantiation(map_tmpl, args));

    return lookup;
}
