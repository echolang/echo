#include "AST/ASTCollectionLiteral.h"

#include "AST/ASTArgumentFit.h"
#include "AST/ASTArrayLiteral.h"
#include "AST/ASTCollector.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTMapLiteral.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTTypeUnify.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"

#include <cassert>
#include <optional>
#include <vector>


namespace
{
    AST::ValueType peel_substituted(
        const AST::FunctionDeclNode &op,
        const AST::ValueType &declared,
        const AST::ValueType &receiver,
        AST::Collector &collector
    )
    {
        AST::TypeSubstitution subst;

        if (!AST::unify_type(AST::operator_receiver_type(op), AST::implicit_conversion_target(receiver), subst)) {
            return AST::ValueType::make_unknown();
        }

        return AST::implicit_conversion_target(
            AST::substitute_type(declared, subst, collector.type_registry));
    }

    // empty `[]` at a keyed dest is constructor-only. a non-empty array is the wrong brackets
    bool collection_fill_matches_shape(
        const AST::CollectionFill &fill,
        const AST::CollectionLiteralExprNode &shape
    )
    {
        if (fill.kind == AST::CollectionFill::Kind::t_none) {
            return false;
        }

        if (shape.get_node_type() == AST::NodeType::n_expr_array_literal
            && fill.kind == AST::CollectionFill::Kind::t_keyed) {
            return static_cast<const AST::ArrayLiteralExprNode &>(shape).elements.empty();
        }

        return true;
    }

    bool fill_rejects(
        const AST::CollectionFill &fill,
        AST::Collector &collector,
        AST::CollectionLiteralExprNode &literal
    );

    // a settled entry that cannot sit in `wanted`. untyped dest-types via argument_fit
    bool fill_rejects_entry(
        AST::ExprNode *entry,
        const AST::ValueType &wanted,
        AST::Collector &collector
    )
    {
        entry = AST::peel_move(entry);
        if (entry == nullptr) {
            return false;
        }

        AST::CollectionLiteralExprNode *inner = AST::collection_literal_of(entry);
        if (inner != nullptr && !inner->expansion_decided) {
            if (AST::contains_type_param(wanted) || AST::is_undetermined_type(wanted)) {
                return false;
            }

            if (std::optional<AST::CollectionBind> bound =
                    AST::collection_bind_type(wanted, collector, *inner)) {
                return fill_rejects(bound->fill, collector, *inner);
            }

            return AST::type_is_determined_for_fit(AST::implicit_conversion_target(wanted));
        }

        const AST::ValueType from = entry->result_type();
        if (!AST::type_is_determined_for_fit(from) || !AST::type_is_determined_for_fit(wanted)) {
            return false;
        }

        return AST::argument_fit(from, entry, wanted) == AST::ArgumentFit::t_none;
    }

    bool fill_rejects(
        const AST::CollectionFill &fill,
        AST::Collector &collector,
        AST::CollectionLiteralExprNode &literal
    )
    {
        if (AST::ArrayLiteralExprNode *array = AST::array_literal_of(&literal)) {
            if (fill.kind != AST::CollectionFill::Kind::t_inline
                && fill.kind != AST::CollectionFill::Kind::t_append
                && fill.kind != AST::CollectionFill::Kind::t_index) {
                return false;
            }

            for (AST::ExprNode *element : array->elements) {
                if (fill_rejects_entry(element, fill.element, collector)) {
                    return true;
                }
            }

            return false;
        }

        AST::MapLiteralExprNode *map = AST::map_literal_of(&literal);
        if (map == nullptr || fill.kind != AST::CollectionFill::Kind::t_keyed) {
            return false;
        }

        for (const AST::MapLiteralExprNode::Entry &entry : map->entries) {
            if (fill_rejects_entry(entry.key, fill.key, collector)
                || fill_rejects_entry(entry.value, fill.value, collector)) {
                return true;
            }
        }

        return false;
    }

    std::optional<AST::CollectionBind> bind_at(
        const AST::ValueType &source,
        AST::Collector &collector,
        AST::CollectionLiteralExprNode &literal
    )
    {
        const AST::CollectionFill fill = AST::collection_fill_of(source, collector, literal);
        if (!collection_fill_matches_shape(fill, literal) || fill_rejects(fill, collector, literal)) {
            return std::nullopt;
        }

        AST::CollectionBind bound;
        bound.type = source;
        bound.fill = fill;
        return bound;
    }

    // dest-type at each inbound source. several survivors are none, not infer-then-exact.
    // zero inbounds still asks find_implicit_conversion of the inferred collection (outbound)
    std::optional<AST::CollectionBind> collection_conversion_source(
        const AST::ValueType &destination,
        AST::Collector &collector,
        AST::CollectionLiteralExprNode &literal
    )
    {
        if (!destination.has_complex_type()) {
            return std::nullopt;
        }

        const AST::ComplexType *owner = destination.get_complex_type();
        if (owner == nullptr) {
            return std::nullopt;
        }

        const AST::ComplexType *published = owner->template_or_self();
        if (published == nullptr) {
            return std::nullopt;
        }

        std::optional<AST::CollectionBind> survivor;
        bool several = false;

        for (AST::FunctionDeclNode *candidate : published->implicit_conversions()) {
            if (candidate == nullptr || candidate->args.size() != 1) {
                continue;
            }

            const AST::ValueType source = candidate->parameter_type(0);
            if (AST::find_inbound_implicit_conversion(owner, source) != candidate) {
                continue;
            }

            std::optional<AST::CollectionBind> bound = bind_at(source, collector, literal);
            if (!bound.has_value()) {
                continue;
            }

            if (survivor.has_value()) {
                several = true;
                break;
            }

            survivor = std::move(bound);
        }

        if (several) {
            return std::nullopt;
        }

        if (survivor.has_value()) {
            return survivor;
        }

        const AST::CollectionLiteralLookup look =
            AST::collection_literal_type_for(literal, collector.core_types, collector.type_registry);

        if (look.result != AST::CollectionLiteralLookup::Result::t_ok) {
            return std::nullopt;
        }

        if (AST::find_implicit_conversion(look.type, destination) == nullptr) {
            return std::nullopt;
        }

        return bind_at(look.type, collector, literal);
    }
}

AST::CollectionLiteralExprNode *AST::collection_literal_of(AST::ExprNode *expr)
{
    if (ArrayLiteralExprNode *array = array_literal_of(expr)) {
        return array;
    }

    return map_literal_of(expr);
}

AST::CollectionFill AST::collection_fill_of(
    const AST::ValueType &destination,
    AST::Collector &collector,
    const AST::CollectionLiteralExprNode &shape
)
{
    CollectionFill fill;
    const ValueType wanted = implicit_conversion_target(destination);
    const bool keyed_shape = shape.get_node_type() == NodeType::n_expr_map_literal;

    const auto try_keyed = [&] {
        if (const FunctionDeclNode *write = find_index_write(collector, wanted, 1)) {
            fill.kind = CollectionFill::Kind::t_keyed;
            fill.key = peel_substituted(*write, write->parameter_type(1), wanted, collector);
            fill.value = peel_substituted(*write, write->parameter_type(2), wanted, collector);
        }
    };

    if (keyed_shape) {
        try_keyed();
        return fill;
    }

    if (wanted.is_inline_array()) {
        fill.kind = CollectionFill::Kind::t_inline;
        fill.element = wanted.array_element();
        fill.length = wanted.bound_array_length();
        return fill;
    }

    if (!wanted.has_property_layout()) {
        return fill;
    }

    ComplexType *ct = wanted.get_complex_type();
    const ComplexType *tmpl = ct->template_or_self();
    const ComplexType *fixed_tmpl = collector.core_types.declared_template(CoreTypeKind::t_fixed_array);

    if (fixed_tmpl != nullptr && tmpl == fixed_tmpl) {
        for (size_t i = 0; i < ct->property_count(); i++) {
            const ValueType &field = ct->get_property_type(i);

            if (field.is_inline_array()) {
                fill.kind = CollectionFill::Kind::t_index;
                fill.element = field.array_element();
                fill.length = field.bound_array_length();
                return fill;
            }
        }

        return fill;
    }

    if (const FunctionDeclNode *write = find_index_write(collector, wanted, 0)) {
        fill.kind = CollectionFill::Kind::t_append;
        fill.element = peel_substituted(*write, write->parameter_type(1), wanted, collector);
        return fill;
    }

    if (const FunctionDeclNode *read = find_index(collector, wanted, 0)) {
        fill.kind = CollectionFill::Kind::t_append;
        fill.element = peel_substituted(*read, read->get_return_type(), wanted, collector);
        return fill;
    }

    // keyed is the array-shape fallback: a dest that also appends returned above
    try_keyed();
    return fill;
}

std::optional<AST::CollectionBind> AST::collection_bind_type(
    const ValueType &destination,
    Collector &collector,
    CollectionLiteralExprNode &literal
)
{
    const ValueType wanted = implicit_conversion_target(destination);

    if (is_undetermined_type(wanted)) {
        return std::nullopt;
    }

    const CollectionFill fill = collection_fill_of(wanted, collector, literal);
    if (collection_fill_matches_shape(fill, literal)) {
        CollectionBind bound;
        bound.type = wanted;
        bound.fill = fill;
        return bound;
    }

    return collection_conversion_source(wanted, collector, literal);
}

bool AST::collection_fill_rejects(
    const CollectionFill &fill,
    Collector &collector,
    CollectionLiteralExprNode &literal
)
{
    return fill_rejects(fill, collector, literal);
}

bool AST::bind_collection_literal_to(
    AST::ExprNode *expr,
    const AST::ValueType &destination,
    AST::Collector &collector
)
{
    if (bind_array_literal_to(expr, destination, collector)) {
        return true;
    }

    return bind_map_literal_to(expr, destination, collector);
}

AST::CollectionLiteralLookup AST::collection_entry_type_for(
    AST::ExprNode *entry,
    const AST::CoreTypes &core,
    AST::TypeRegistry &types
)
{
    if (auto *inner = array_literal_of(entry)) {
        return array_literal_type_for(*inner, core, types);
    }

    if (auto *inner = map_literal_of(entry)) {
        return map_literal_type_for(*inner, core, types);
    }

    if (entry == nullptr) {
        return {};
    }

    const AST::ValueType got = AST::value_result_type(*entry);

    if (AST::is_undetermined_type(got)) {
        return {};
    }

    CollectionLiteralLookup ok;
    ok.result = CollectionLiteralLookup::Result::t_ok;
    ok.type = got;
    return ok;
}

AST::CollectionLiteralLookup AST::collection_literal_type_for(
    AST::CollectionLiteralExprNode &literal,
    const AST::CoreTypes &core,
    AST::TypeRegistry &types
)
{
    if (auto *array = array_literal_of(&literal)) {
        return array_literal_type_for(*array, core, types);
    }

    auto *map = map_literal_of(&literal);
    assert(map != nullptr);
    return map_literal_type_for(*map, core, types);
}
