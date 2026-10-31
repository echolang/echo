#include "AST/ASTCollectionLiteralExpansion.h"

#include "AST/ASTArrayLiteral.h"
#include "AST/ASTCodeRef.h"
#include "AST/ASTCollector.h"
#include "AST/ASTCollectionLiteral.h"
#include "AST/ASTConstFold.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTDetach.h"
#include "AST/ASTIssue.h"
#include "AST/ASTLiteralTyping.h"
#include "AST/ASTMapLiteral.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTModule.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/AssignNode.h"
#include "AST/ConstantDataExprNode.h"
#include "AST/ExprNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"

#include <cassert>
#include <fmt/core.h>
#include <optional>
#include <string>

namespace AST
{

CollectionLiteralExpansion::CollectionLiteralExpansion(
    Module &module,
    Collector &collector,
    File *file,
    bool finalizing,
    size_t &hoist_count,
    DetachBatch &detached
)
    : _module(module)
    , _collector(collector)
    , _file(file)
    , _finalizing(finalizing)
    , _hoist_count(hoist_count)
    , _detached(detached)
{
}

CodeRef CollectionLiteralExpansion::code_ref_for(const TokenReference &token)
{
    return CodeRef{&_module, token.make_slice()};
}

CollectionLiteralExpansion::Destination CollectionLiteralExpansion::destination_of(Node *statement)
{
    if (statement == nullptr) {
        return {};
    }

    // a declaration's initializer: `array<int32> $a = [1, 2, 3];` / `map<K, V> $m = ["a" => 1];`
    if (statement->get_node_type() == NodeType::n_vardecl) {
        auto *decl = static_cast<VarDeclNode *>(statement);
        CollectionLiteralExprNode *literal = collection_literal_of(decl->init_expr);

        if (literal == nullptr) {
            return {};
        }

        return {decl, literal, &decl->init_expr, true};
    }

    // an assignment's right-hand side. a plain variable is filled in place: `$a = [1, 2, 3];`.
    // any other target - `$a[] = [7, 9]`, `$s->items = [...]` - cannot be: the expansion needs
    // one fresh place per append, and cloning the target subtree per element is A13b's temporary
    // problem wearing different clothes. those keep the slot so the hoist can replace the literal
    if (statement->get_node_type() == NodeType::n_assign) {
        auto *assign = static_cast<AssignNode *>(statement);
        CollectionLiteralExprNode *literal = collection_literal_of(assign->value_expr);

        if (literal == nullptr) {
            return {};
        }

        if (assign->target == nullptr || assign->target->get_node_type() != NodeType::n_varref) {
            return {nullptr, literal, &assign->value_expr, false, assign->target};
        }

        return {place_root_of(assign->target), literal, &assign->value_expr};
    }

    return {};
}

void CollectionLiteralExpansion::report_unplaced(CollectionLiteralExprNode &literal)
{
    if (literal.expansion_decided) {
        return;
    }

    literal.expansion_decided = true;

    const char *message = literal.get_node_type() == NodeType::n_expr_map_literal
        ? "a map literal fills storage, so it has to name it - write it as a declaration's "
          "initializer or as an assignment to a variable."
        : "an array literal fills storage, so it has to name it - write it as a declaration's "
          "initializer or as an assignment to a variable.";

    _collector.collect_issue<Issue::GenericError>(
        code_ref_for(literal.token_bracket),
        message);
}

bool CollectionLiteralExpansion::settle_destination_type(
    const Destination &destination,
    ValueType &settled
)
{
    ValueType destination_type =
        destination.decl->has_type() ? destination.decl->type() : ValueType::make_unknown();

    // **the declaration said nothing about what holds these, so the entries are asked.**
    // the two `*_type_for` owners answer that; collection_literal_type_for is the dispatch
    if (destination.declares && is_undetermined_type(destination_type)) {
        const CollectionLiteralLookup look = collection_literal_type_for(
            *destination.literal, _collector.core_types, _collector.type_registry);

        if (look.result == CollectionLiteralLookup::Result::t_refused) {
            destination.literal->expansion_decided = true;

            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(destination.decl->token_varname), look.refusal);

            return false;
        }

        if (look.result == CollectionLiteralLookup::Result::t_ok) {
            destination_type = infer_declaration_type(look.type, destination_type.is_const());

            destination.decl->set_type_node(&_module.nodes.emplace_back<TypeNode>(destination_type));
        }
    }

    // not decided yet: the declaration may be typed from a call this fixpoint has not settled, or an
    // entry above may be. out of rounds is out of answers
    if (is_undetermined_type(destination_type)) {
        if (_finalizing) {
            destination.literal->expansion_decided = true;

            if (!_collector.has_critical_issues()) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(destination.literal->token_bracket),
                    fmt::format(
                        "nothing ever said what '{}' holds - a {} literal takes its type from "
                        "where it is going, and this destination never became concrete.",
                        destination.decl->token_varname.value(),
                        destination.literal->kind_spelling()));
            }
        }

        return false;
    }

    settled = destination_type;
    return true;
}

bool CollectionLiteralExpansion::bind_unplaced_type(CollectionLiteralExprNode &literal)
{
    if (literal.bound_type.has_value()) {
        return true;
    }

    const CollectionLiteralLookup look =
        collection_literal_type_for(literal, _collector.core_types, _collector.type_registry);

    if (look.result == CollectionLiteralLookup::Result::t_refused) {
        literal.expansion_decided = true;

        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(literal.token_bracket), look.refusal);

        return false;
    }

    if (look.result != CollectionLiteralLookup::Result::t_ok) {
        if (_finalizing) {
            report_unplaced(literal);
        }

        return false;
    }

    literal.bound_type = look.type;
    return true;
}

bool CollectionLiteralExpansion::mint_constructor(
    CollectionLiteralExprNode &literal,
    const ValueType &type,
    ExprNode **slot
)
{
    ComplexType *ct = type.has_property_layout() ? type.get_complex_type() : nullptr;

    if (ct == nullptr) {
        return false;
    }

    const ComplexType *tmpl = ct->template_or_self();

    std::vector<TypeNode *> type_args;
    for (const auto &arg : ct->instantiation_args) {
        type_args.push_back(&_module.nodes.emplace_back<TypeNode>(arg));
    }

    auto &ctor = build_named_call(
        tmpl->name.value_or(std::string()),
        literal.token_bracket,
        tmpl->ast_namespace != nullptr ? tmpl->ast_namespace : &_collector.namespaces.root());

    ctor.explicit_type_args = std::move(type_args);
    *slot = &ctor;
    return true;
}

void CollectionLiteralExpansion::emit_index_assign(
    CollectionLiteralExprNode &literal,
    VarDeclNode &into,
    std::vector<ExprNode *> indices,
    ExprNode *value,
    std::vector<NodeReference> &appends
)
{
    auto &var_ref = local_place(_module, into);

    auto &slot_expr = _module.nodes.emplace_back<IndexExprNode>(
        &var_ref, std::move(indices), literal.token_bracket);

    // the three facts the parser records for a hand-written `$a[] = v` / `$a[$i] = v`,
    // recorded here for the same reasons: the slot is bound rather than read, there is an
    // `=` behind the bracket, and the write into it initializes storage that holds nothing,
    // so no teardown is owed
    slot_expr.slot_is_bound = true;
    slot_expr.is_assignment_target = true;

    auto &assign = _module.nodes.emplace_back<AssignNode>(
        &slot_expr, value, literal.token_bracket);
    assign.is_initialization = true;

    appends.push_back(make_ref(assign));
}

bool CollectionLiteralExpansion::build_expansion(
    CollectionLiteralExprNode &literal,
    VarDeclNode &into,
    const ValueType &type,
    ExprNode **slot,
    std::vector<NodeReference> &appends
)
{
    ArrayLiteralExprNode *array = array_literal_of(&literal);
    MapLiteralExprNode *map = map_literal_of(&literal);
    const CollectionFill fill = collection_fill_of(type, _collector, literal);

    // nested dest-typing lives on bind_*, which reads the fill contract. expansion asks once
    bind_collection_literal_to(&literal, type, _collector);

    if (array != nullptr) {
        if (fill.kind == CollectionFill::Kind::t_keyed) {
            if (!array->elements.empty()) {
                array->expansion_decided = true;
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(array->token_bracket),
                    fmt::format(
                        "'{}' cannot be built from an array literal - write '[key => value]' to fill a map.",
                        type.get_type_desciption()));
                return false;
            }

            // empty `[]` at a keyed dest is constructor-only: `map<K, V> $m = []`
            return mint_constructor(*array, type, slot);
        }

        if (fill.kind == CollectionFill::Kind::t_none) {
            array->expansion_decided = true;
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(array->token_bracket),
                fmt::format(
                    "'{}' cannot be built from an array literal - a literal fills a collection, through "
                    "its constructor and its append operator.",
                    type.get_type_desciption()));
            return false;
        }

        if (fill.kind == CollectionFill::Kind::t_inline) {
            // the local is already storage; a constructor would be a second answer to how T[N]
            // starts. null here is the zero-init gen_var_decl already does
            *slot = nullptr;
        } else if (!mint_constructor(*array, type, slot)) {
            return false;
        }

        if (fill.kind == CollectionFill::Kind::t_inline || fill.kind == CollectionFill::Kind::t_index) {
            if (!fill.length.has_value()) {
                return false;
            }

            if (array->elements.size() != *fill.length) {
                array->expansion_decided = true;
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(array->token_bracket),
                    fmt::format(
                        "this literal has {} elements, '{}' has {}",
                        array->elements.size(), type.get_type_desciption(), *fill.length));
                return false;
            }
        }

        if (fill.kind == CollectionFill::Kind::t_append
            && fill_from_constant_data(*array, into, type, fill, appends)) {
            return true;
        }

        appends.reserve(array->elements.size());

        for (size_t i = 0; i < array->elements.size(); i++) {
            std::vector<ExprNode *> indices;

            if (fill.kind == CollectionFill::Kind::t_inline || fill.kind == CollectionFill::Kind::t_index) {
                const TokenReference index_token = _module.make_virtual_token(
                    std::to_string(i), Token::Type::t_integer_literal, array->token_bracket);
                auto &index = _module.nodes.emplace_back<LiteralIntExprNode>(
                    index_token, ValueTypePrimitive::t_usize);
                indices.push_back(&index);
            }

            emit_index_assign(*array, into, std::move(indices), array->elements[i], appends);
        }

        return true;
    }

    assert(map != nullptr);

    if (fill.kind != CollectionFill::Kind::t_keyed) {
        map->expansion_decided = true;
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(map->token_bracket),
            fmt::format(
                "'{}' cannot be built from a map literal - a literal fills a collection, through "
                "its constructor and its index-write operator.",
                type.get_type_desciption()));
        return false;
    }

    if (!mint_constructor(*map, type, slot)) {
        map->expansion_decided = true;
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(map->token_bracket),
            fmt::format(
                "'{}' cannot be built from a map literal - a literal fills a collection, through "
                "its constructor and its index-write operator.",
                type.get_type_desciption()));
        return false;
    }

    appends.reserve(map->entries.size());

    for (MapLiteralExprNode::Entry &entry : map->entries) {
        emit_index_assign(*map, into, {entry.key}, entry.value, appends);
    }

    return true;
}

// below this, appends are a few stores in registers. above it, they are calls to settle and, inlined,
// one block the scheduler is quadratic in: the Unicode case tables were 80% of a cold stdlib compile
static constexpr size_t k_constant_data_min_elements = 16;

bool CollectionLiteralExpansion::fill_from_constant_data(
    ArrayLiteralExprNode &literal,
    VarDeclNode &into,
    const ValueType &type,
    const CollectionFill &fill,
    std::vector<NodeReference> &appends
)
{
    const ComplexType *array_template = _collector.core_types.declared_template(CoreTypeKind::t_array);
    Namespace *arr = _collector.namespaces.get("arr");
    ComplexType *ct = type.has_property_layout() ? type.get_complex_type() : nullptr;

    if (literal.elements.size() < k_constant_data_min_elements
        || array_template == nullptr
        || arr == nullptr
        || ct == nullptr
        || ct->template_or_self() != array_template) {
        return false;
    }

    const ValueType &element = fill.element;

    if (!element.is_integer_type() || element.is_const()) {
        return false;
    }

    std::vector<uint64_t> bits;
    bits.reserve(literal.elements.size());

    for (ExprNode *&written : literal.elements) {
        // a refusal belongs to the appends, at this same literal
        const LiteralTyping typing = type_literal_at(written, element, _module.nodes);

        if (typing.result == LiteralTyping::Result::t_refused) {
            return false;
        }

        if (typing.result == LiteralTyping::Result::t_typed) {
            written = typing.node;
        }

        const ConstFoldResult folded = const_fold(written);

        if (!folded.is_folded() || ValueType::make_mutable(folded.type) != element) {
            return false;
        }

        bits.push_back(folded.bits);
    }

    for (ExprNode *written : literal.elements) {
        _detached.collect(*written);
    }

    literal.elements.clear();

    const size_t count = bits.size();

    auto &data = _module.nodes.emplace_back<ConstantDataExprNode>(
        literal.token_bracket, element, std::move(bits));

    auto &length = _module.nodes.emplace_back<LiteralIntExprNode>(
        _module.make_virtual_token(
            std::to_string(count), Token::Type::t_integer_literal, literal.token_bracket),
        ValueTypePrimitive::t_usize);

    // named, not resolved: InterpolationLowering's trick
    auto &fill_call = build_named_call("extend_from", literal.token_bracket, arr);

    fill_call.arguments = { &local_place(_module, into), &data, &length };

    appends.push_back(make_ref(fill_call));
    return true;
}

FunctionCallExprNode &CollectionLiteralExpansion::build_named_call(
    const std::string &name,
    const TokenReference &at,
    const Namespace *lookup
)
{
    const TokenReference name_token =
        _module.make_virtual_token(name, Token::Type::t_identifier, at);

    auto &call = _module.nodes.emplace_back<FunctionCallExprNode>(name_token, std::vector<ExprNode *>{});

    call.lookup_namespace = lookup;

    return call;
}

ExprNode *CollectionLiteralExpansion::hoist(
    CollectionLiteralExprNode &literal,
    std::vector<NodeReference> &hoisted
)
{
    if (!literal.bound_type.has_value()) {
        return nullptr;
    }

    literal.expansion_decided = true;

    // **the temp is always mutable.** append and keyed write take a mutable borrow; a const dest
    // (generated ctor field, `take(const array<T>)`) takes the filled value afterwards
    const ValueType fill_type = ValueType::make_mutable(*literal.bound_type);

    auto &decl = _module.nodes.emplace_back<VarDeclNode>(
        _module.make_virtual_token(
            fmt::format("$__lit{}", ++_hoist_count), Token::Type::t_varname, literal.token_bracket),
        &_module.nodes.emplace_back<TypeNode>(fill_type));
    decl.statement_temp = true;

    std::vector<NodeReference> appends;

    if (!build_expansion(literal, decl, fill_type, &decl.init_expr, appends)) {
        return nullptr;
    }

    hoisted.push_back(make_ref(decl));
    hoisted.insert(hoisted.end(), appends.begin(), appends.end());

    return &local_place(_module, decl);
}

bool CollectionLiteralExpansion::expand_statement(
    ScopeNode &scope,
    size_t index,
    std::vector<NodeReference> &hoisted
)
{
    const Destination destination = destination_of(scope.children[index].node());

    if (destination.literal == nullptr || destination.literal->expansion_decided) {
        return false;
    }

    if (destination.assign_target != nullptr) {
        const ValueType dest = value_result_type(*destination.assign_target);
        if (!is_undetermined_type(dest)) {
            bind_collection_literal_to(*destination.slot, dest, _collector);
        }
    }

    // **mutable variable storage is filled in place.** everything else hoists: a const name cannot
    // be the append / write target, and a non-variable target would have to be cloned per element
    if (destination.decl != nullptr) {
        ValueType destination_type;

        if (!settle_destination_type(destination, destination_type)) {
            return false;
        }

        if (!destination_type.is_const()) {
            std::vector<NodeReference> appends;
            destination.literal->expansion_decided = true;

            if (!build_expansion(
                    *destination.literal,
                    *destination.decl,
                    destination_type,
                    destination.slot,
                    appends)) {
                return false;
            }

            scope.children.insert(scope.children.begin() + index + 1, appends.begin(), appends.end());
            return true;
        }

        destination.literal->bound_type = ValueType::make_mutable(destination_type);
    } else {
        if (destination.slot == nullptr) {
            report_unplaced(*destination.literal);
            return false;
        }

        if (!bind_unplaced_type(*destination.literal)) {
            return false;
        }
    }

    ExprNode *place = hoist(*destination.literal, hoisted);

    if (place == nullptr) {
        return false;
    }

    *destination.slot = &_module.nodes.emplace_back<MoveExprNode>(
        place, destination.literal->token_bracket);
    return true;
}

ExprNode *CollectionLiteralExpansion::expand_expression(
    ExprNode *expr,
    std::vector<NodeReference> &hoisted
)
{
    CollectionLiteralExprNode *literal = collection_literal_of(expr);

    if (literal == nullptr || literal->expansion_decided) {
        return nullptr;
    }

    // a clone of a constant's array literal has no destination of its own. infer from
    // the first element so MONTH_DAYS[$i] can hoist the same way an argument literal does
    if (!bind_unplaced_type(*literal)) {
        return nullptr;
    }

    ExprNode *place = hoist(*literal, hoisted);

    if (place != nullptr) {
        return place;
    }

    if (_finalizing) {
        report_unplaced(*literal);
    }

    return nullptr;
}

void place_collection_literal_hoists(
    ScopeNode &scope,
    size_t index,
    Module &module,
    std::vector<NodeReference> &hoisted
)
{
    Node *statement = scope.children[index].node();

    // a declaration whose initializer contained the literal has to outlive the hoist. wrapping
    // both scoped the declared name to the hoist, so `$b = Bag([7, 9])` destroyed `$b` at the
    // closing brace. a `guard` binding is the same: wrapping would drop an owning `$o` at the wrapper's brace.
    // every other statement still wraps: `$a->push([1, 2])` must not hold the temp until the frame ends
    const NodeType type = statement != nullptr ? statement->get_node_type() : NodeType::n_void;

    if (type == NodeType::n_vardecl || type == NodeType::n_guard) {
        scope.children.insert(scope.children.begin() + index, hoisted.begin(), hoisted.end());
        hoisted.clear();
        return;
    }

    auto &wrapper = module.nodes.emplace_back<ScopeNode>();
    wrapper.parent_ptr = &scope;

    wrapper.children = std::move(hoisted);
    wrapper.children.push_back(scope.children[index]);

    hoisted.clear();

    scope.children[index] = make_ref(wrapper);
}

};  // namespace AST
