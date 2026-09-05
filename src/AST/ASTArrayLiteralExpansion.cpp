#include "AST/ASTArrayLiteralExpansion.h"

#include "AST/ASTArrayLiteral.h"
#include "AST/ASTCodeRef.h"
#include "AST/ASTCollector.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTIssue.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTModule.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"

#include <fmt/core.h>
#include <optional>
#include <string>

namespace AST
{

ArrayLiteralExpansion::ArrayLiteralExpansion(
    Module &module,
    Collector &collector,
    File *file,
    bool finalizing,
    size_t &hoist_count
)
    : _module(module)
    , _collector(collector)
    , _file(file)
    , _finalizing(finalizing)
    , _hoist_count(hoist_count)
{
}

CodeRef ArrayLiteralExpansion::code_ref_for(const TokenReference &token)
{
    return CodeRef{&_module, token.make_slice()};
}

ArrayLiteralExpansion::Destination ArrayLiteralExpansion::destination_of(Node *statement)
{
    if (statement == nullptr) {
        return {};
    }

    // a declaration's initializer: `array<int32> $a = [1, 2, 3];`
    if (statement->get_node_type() == NodeType::n_vardecl) {
        auto *decl = static_cast<VarDeclNode *>(statement);

        if (auto *literal = array_literal_of(decl->init_expr)) {
            return {decl, literal, &decl->init_expr, true};
        }

        return {};
    }

    // an assignment's right-hand side. a plain variable is filled in place: `$a = [1, 2, 3];`.
    // any other target - `$a[] = [7, 9]`, `$s->items = [...]` - cannot be: the expansion needs
    // one fresh place per append, and cloning the target subtree per element is A13b's temporary
    // problem wearing different clothes. those keep the slot so the hoist can replace the literal
    if (statement->get_node_type() == NodeType::n_assign) {
        auto *assign = static_cast<AssignNode *>(statement);
        auto *literal = array_literal_of(assign->value_expr);

        if (literal == nullptr) {
            return {};
        }

        if (assign->target == nullptr || assign->target->get_node_type() != NodeType::n_varref) {
            return {nullptr, literal, &assign->value_expr};
        }

        return {place_root_of(assign->target), literal, &assign->value_expr};
    }

    return {};
}

void ArrayLiteralExpansion::report_unplaced(ArrayLiteralExprNode &literal)
{
    if (literal.expansion_decided) {
        return;
    }

    literal.expansion_decided = true;

    _collector.collect_issue<Issue::GenericError>(
        code_ref_for(literal.token_bracket),
        "an array literal fills storage, so it has to name it - write it as a declaration's "
        "initializer or as an assignment to a variable.");
}

bool ArrayLiteralExpansion::settle_destination_type(
    const Destination &destination,
    ValueType &settled
)
{
    ArrayLiteralExprNode &literal = *destination.literal;

    ValueType destination_type =
        destination.decl->has_type() ? destination.decl->type() : ValueType::make_unknown();

    // **the declaration said nothing about what holds these, so the elements are asked.**
    // AST::array_literal_type_for owns that question and answers three ways, so `[f(), g()]` is asked
    // again next round rather than refused on the first
    if (destination.declares && is_undetermined_type(destination_type)) {
        const ArrayLiteralLookup look =
            array_literal_type_for(literal, _collector.core_types, _collector.type_registry);

        if (look.result == ArrayLiteralLookup::Result::t_refused) {
            literal.expansion_decided = true;

            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(destination.decl->token_varname), look.refusal);

            return false;
        }

        if (look.result == ArrayLiteralLookup::Result::t_ok) {
            // the `const` the declaration was written with, put back on top - the same half
            // AST::infer_declaration_type applies at the two other moments
            destination_type = infer_declaration_type(look.type, destination_type.is_const());

            destination.decl->set_type_node(&_module.nodes.emplace_back<TypeNode>(destination_type));
        }
    }

    // not decided yet: the declaration may be typed from a call this fixpoint has not settled, or an
    // element above may be. out of rounds is out of answers
    if (is_undetermined_type(destination_type)) {
        if (_finalizing) {
            literal.expansion_decided = true;

            if (!_collector.has_critical_issues()) {
                _collector.collect_issue<Issue::GenericError>(
                    code_ref_for(literal.token_bracket),
                    fmt::format(
                        "nothing ever said what '{}' holds - an array literal takes its type from "
                        "where it is going, and this destination never became concrete.",
                        destination.decl->token_varname.value()));
            }
        }

        return false;
    }

    settled = destination_type;
    return true;
}

bool ArrayLiteralExpansion::bind_unplaced_type(ArrayLiteralExprNode &literal)
{
    if (literal.bound_type.has_value()) {
        return true;
    }

    const ArrayLiteralLookup look =
        array_literal_type_for(literal, _collector.core_types, _collector.type_registry);

    if (look.result == ArrayLiteralLookup::Result::t_refused) {
        literal.expansion_decided = true;

        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(literal.token_bracket), look.refusal);

        return false;
    }

    if (look.result != ArrayLiteralLookup::Result::t_ok) {
        if (_finalizing) {
            report_unplaced(literal);
        }

        return false;
    }

    literal.bound_type = look.type;
    return true;
}

bool ArrayLiteralExpansion::build_expansion(
    ArrayLiteralExprNode &literal,
    VarDeclNode &into,
    const ValueType &type,
    ExprNode **slot,
    std::vector<NodeReference> &appends
)
{
    // `T[N]` is storage, not a collection: zero-filled, then one indexed write per element.
    // no constructor to call, and the length is the type's rather than a core-type argument
    const bool fills_inline = type.is_inline_array();

    ComplexType *ct = type.has_property_layout() ? type.get_complex_type() : nullptr;
    const ComplexType *tmpl = ct != nullptr ? ct->template_or_self() : nullptr;

    if (!fills_inline && ct == nullptr) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(literal.token_bracket),
            fmt::format(
                "'{}' cannot be built from an array literal - a literal fills a collection, through "
                "its constructor and its append operator.",
                type.get_type_desciption()));
        return false;
    }

    const ComplexType *fixed_tmpl = _collector.core_types.declared_template(CoreTypeKind::t_fixed_array);
    const bool fills_by_index = fills_inline
        || (fixed_tmpl != nullptr && tmpl == fixed_tmpl);

    // length is always a T[N] question. the destination itself, or the inline-array field a
    // `fixed_array` wraps - never instantiation_args[1], which is a shape this pass does not own
    std::optional<uint64_t> length;
    if (fills_inline) {
        length = type.bound_array_length();
        // the local is already storage; a constructor would be a second answer to how T[N]
        // starts. null here is the zero-init gen_var_decl already does
        *slot = nullptr;
    } else {
        // **the constructor of the destination type, named rather than looked up.** an instantiation
        // carries its template's name and its own type arguments, which is exactly what a call site
        // writes as `array<int32>()`
        std::vector<TypeNode *> type_args;
        for (const auto &arg : ct->instantiation_args) {
            type_args.push_back(&_module.nodes.emplace_back<TypeNode>(arg));
        }

        auto &ctor = build_constructor_call(
            tmpl->name.value_or(std::string()),
            literal.token_bracket,
            tmpl->ast_namespace != nullptr ? tmpl->ast_namespace : &_collector.namespaces.root());

        ctor.explicit_type_args = std::move(type_args);

        *slot = &ctor;

        if (fills_by_index) {
            for (size_t i = 0; i < ct->property_count(); i++) {
                const ValueType &field = ct->get_property_type(i);
                if (field.is_inline_array()) {
                    length = field.bound_array_length();
                    break;
                }
            }
        }
    }

    if (fills_by_index) {
        if (!length.has_value()) {
            return false;
        }

        if (literal.elements.size() != *length) {
            literal.expansion_decided = true;
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(literal.token_bracket),
                fmt::format(
                    "this literal has {} elements, '{}' has {}",
                    literal.elements.size(), type.get_type_desciption(), *length));
            return false;
        }
    }

    // one write per element. each gets its **own** place naming the destination - one
    // subtree per append, because PointerAdjuster rewrites edges in place and a shared one would be
    // adjusted once per use
    appends.reserve(literal.elements.size());

    for (size_t i = 0; i < literal.elements.size(); i++) {
        ExprNode *element = literal.elements[i];
        auto &var_ref = local_place(_module, into);

        std::vector<ExprNode *> indices;
        if (fills_by_index) {
            const TokenReference index_token = _module.make_virtual_token(
                std::to_string(i), Token::Type::t_integer_literal, literal.token_bracket);
            auto &index = _module.nodes.emplace_back<LiteralIntExprNode>(
                index_token, ValueTypePrimitive::t_usize);
            indices.push_back(&index);
        }

        auto &slot_expr = _module.nodes.emplace_back<IndexExprNode>(
            &var_ref, std::move(indices), literal.token_bracket);

        // the three facts the parser records for a hand-written `$a[] = v` / `$a[$i] = v`,
        // recorded here for the same reasons: the slot is bound rather than read, there is an
        // `=` behind the bracket, and the write into it initializes storage that holds nothing,
        // so no teardown is owed
        slot_expr.slot_is_bound = true;
        slot_expr.is_assignment_target = true;

        auto &assign = _module.nodes.emplace_back<AssignNode>(
            &slot_expr, element, literal.token_bracket);
        assign.is_initialization = true;

        appends.push_back(make_ref(assign));
    }

    return true;
}

FunctionCallExprNode &ArrayLiteralExpansion::build_constructor_call(
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

ExprNode *ArrayLiteralExpansion::hoist(
    ArrayLiteralExprNode &literal,
    std::vector<NodeReference> &hoisted,
    size_t hoist_barrier
)
{
    // under a `?->` or a `??`, where hoisting would move the construction above the branch that
    // decides whether it happens at all. refused outright rather than waited on
    if (hoist_barrier > 0) {
        report_unplaced(literal);
        return nullptr;
    }

    if (!literal.bound_type.has_value()) {
        return nullptr;
    }

    literal.expansion_decided = true;

    auto &decl = _module.nodes.emplace_back<VarDeclNode>(
        _module.make_virtual_token(
            fmt::format("$__lit{}", ++_hoist_count), Token::Type::t_varname, literal.token_bracket),
        &_module.nodes.emplace_back<TypeNode>(*literal.bound_type));

    std::vector<NodeReference> appends;

    if (!build_expansion(literal, decl, *literal.bound_type, &decl.init_expr, appends)) {
        return nullptr;
    }

    hoisted.push_back(make_ref(decl));
    hoisted.insert(hoisted.end(), appends.begin(), appends.end());

    return &local_place(_module, decl);
}

bool ArrayLiteralExpansion::expand_statement(
    ScopeNode &scope,
    size_t index,
    std::vector<NodeReference> &hoisted
)
{
    const Destination destination = destination_of(scope.children[index].node());

    if (destination.literal == nullptr || destination.literal->expansion_decided) {
        return false;
    }

    ArrayLiteralExprNode &literal = *destination.literal;

    // **mutable variable storage is filled in place.** everything else hoists: a const name cannot
    // be the append target, and a non-variable target would have to be cloned per element
    if (destination.decl != nullptr) {
        ValueType destination_type;

        if (!settle_destination_type(destination, destination_type)) {
            return false;
        }

        if (!destination_type.is_const()) {
            literal.expansion_decided = true;

            std::vector<NodeReference> appends;

            if (!build_expansion(
                    literal, *destination.decl, destination_type, destination.slot, appends)) {
                return false;
            }

            scope.children.insert(scope.children.begin() + index + 1, appends.begin(), appends.end());
            return true;
        }

        literal.bound_type = ValueType::make_mutable(destination_type);
    } else {
        if (destination.slot == nullptr) {
            report_unplaced(literal);
            return false;
        }

        if (!bind_unplaced_type(literal)) {
            return false;
        }
    }

    ExprNode *place = hoist(literal, hoisted, 0);

    if (place == nullptr) {
        return false;
    }

    *destination.slot = &_module.nodes.emplace_back<MoveExprNode>(place, literal.token_bracket);
    return true;
}

ExprNode *ArrayLiteralExpansion::expand_expression(
    ArrayLiteralExprNode &literal,
    std::vector<NodeReference> &hoisted,
    size_t hoist_barrier
)
{
    if (literal.expansion_decided) {
        return nullptr;
    }

    ExprNode *place = hoist(literal, hoisted, hoist_barrier);

    if (place != nullptr) {
        return place;
    }

    if (_finalizing) {
        report_unplaced(literal);
    }

    return nullptr;
}

void place_array_literal_hoists(
    ScopeNode &scope,
    size_t index,
    Module &module,
    std::vector<NodeReference> &hoisted
)
{
    Node *statement = scope.children[index].node();

    // a declaration whose initializer contained the literal has to outlive the hoist. wrapping
    // both scoped the declared name to the hoist, so `$b = Bag([7, 9])` destroyed `$b` at the
    // closing brace. GuardLowering splices for the same reason. every other statement still wraps:
    // `$a->push([1, 2])` must not hold the temporary until the enclosing frame ends
    if (statement != nullptr && statement->get_node_type() == NodeType::n_vardecl) {
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
