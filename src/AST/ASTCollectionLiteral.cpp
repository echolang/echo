#include "AST/ASTCollectionLiteral.h"

#include "AST/ASTArgumentFit.h"
#include "AST/ASTArrayLiteral.h"
#include "AST/ASTCollector.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTMapLiteral.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTTypeUnify.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"

#include <cassert>


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
