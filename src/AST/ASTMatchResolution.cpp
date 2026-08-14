#include "AST/ASTMatchResolution.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTConstructor.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "AST/ASTModule.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/ASTTypeUnify.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MatchExprNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"

#include <fmt/core.h>

namespace AST
{

MatchResolution::MatchResolution(Bundle &bundle)
    : _bundle(bundle), _collector(bundle.collector)
{
}

CodeRef MatchResolution::code_ref_for(const TokenReference &token)
{
    return CodeRef{_current_module, _current_file, token.make_slice()};
}

bool MatchResolution::run_round()
{
    _changed = false;

    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();

        for (auto &file : module_ptr->files()) {
            _current_file = &file;

            if (file.root != nullptr) {
                file.root->accept(*this);
            }
        }
    }

    return _changed;
}

void MatchResolution::finalize()
{
    _finalizing = true;
    run_round();
    _finalizing = false;
}

void MatchResolution::visitFunctionDecl(FunctionDeclNode &node)
{
    if (!node.is_generic()) {
        RecursiveVisitor::visitFunctionDecl(node);
    }
}

void MatchResolution::visit_match(MatchExprNode &node)
{
    // the subject and the arms first: a match may hold another, and the inner one's subject is settled
    // by the same rounds this one's is. resolving outside-in would answer the outer arms' types from
    // an inner match that has not answered yet
    RecursiveVisitor::visit_match(node);

    if (!node.patterns_decided) {
        resolve(node);
    }
}

void MatchResolution::refuse(MatchExprNode &node, const TokenReference &at, std::string why)
{
    _collector.collect_issue<Issue::GenericError>(code_ref_for(at), std::move(why));

    // **decided, so nothing asks again** - and `void`, so a use site of the value asks nothing further
    // either. the node stays in the tree: its arms are read after it, and forgetting the subtree would
    // free declarations those reads still point at
    node.patterns_decided = true;
    node.result = ValueType::make_void();
}

void MatchResolution::resolve(MatchExprNode &node)
{
    VarDeclNode *subject = node.subject;

    // **the subject's type is derived here rather than by the monomorphizer's stale-variable sweep**,
    // and that is what lets this pass run ahead of the loop and interpolation lowerings rather than
    // behind them. it has to: `"{$s}"` inside an arm is lowered by AST::InterpolationLowering in the
    // first round that reaches it, with no pending state and no finalize, so a binding that is still
    // untyped when it gets there is a `str::from` overload chosen against nothing.
    //
    // the sweep would answer identically - this calls the same AST::infer_declaration_type it does - so
    // the two cannot disagree, and whichever reaches the declaration first settles it. the precedent is
    // beside it: a guard's binding is not that sweep's to derive either
    if (subject != nullptr && subject->init_expr != nullptr
        && (!subject->has_type() || is_undetermined_type(subject->type()))) {
        const ValueType derived = infer_declaration_type(*subject->init_expr, false);

        if (!is_undetermined_type(derived)) {
            auto &type_node = _current_module->nodes.emplace_back<TypeNode>(derived);
            subject->set_type_node(&type_node);
            _changed = true;
        }
    }

    if (subject == nullptr || !subject->has_type() || is_undetermined_type(subject->type())) {
        // not settled yet. **being out of rounds is the proof that it never will be**, which is the
        // only moment this can be said - a round that reported here would report once per round, and
        // the first round's guess would be the diagnostic
        if (_finalizing) {
            refuse(node, node.token,
                "the value this 'match' is over has no settled type, so which enum its patterns name "
                "cannot be worked out");
        }

        return;
    }

    const ValueType subject_type = ValueType::make_mutable(subject->type());

    if (!subject_type.is_enum()) {
        refuse(node, node.token, fmt::format(
            "'match' reads which case an enum is holding, and '{}' is not an enum.",
            subject->type().get_type_desciption()));
        return;
    }

    // the *instantiation* rather than the template: TypeRegistry::get_or_create_instantiation carries
    // the case table across, and the payload property types on it are the substituted ones - which is
    // exactly what a binding of `result<int32, string>::ok($v)` has to hold
    const ComplexType *ct = subject_type.get_complex_type();

    // which arms have already claimed a case, so a second claim is a duplicate rather than a silently
    // dead arm. by ordinal, since two spellings of one case are one case
    std::vector<bool> covered(ct->enum_cases().size(), false);
    bool has_else = false;

    for (MatchExprNode::Arm &arm : node.arms) {
        if (arm.is_else()) {
            if (has_else) {
                refuse(node, arm.token, "this 'match' already has an 'else' arm.");
                return;
            }

            has_else = true;
            continue;
        }

        // an owner written in the pattern has to be the subject's own type. it is allowed to be written
        // at all because it reads better and because a reader wants to see which enum they are matching
        // - but it names nothing new, so disagreeing with the subject is a mistake and not a conversion
        if (arm.owner != nullptr && ValueType::make_mutable(arm.owner->type) != subject_type) {
            refuse(node, arm.token, fmt::format(
                "this 'match' is over '{}', so its patterns name that enum's cases - '{}' is a "
                "different type.",
                subject_type.get_type_desciption(), arm.owner->type.get_type_desciption()));
            return;
        }

        const ComplexType::EnumCase *entry = ct->find_enum_case(arm.case_name);

        if (entry == nullptr) {
            refuse(node, arm.token, fmt::format(
                "'{}' has no case named '{}'.",
                subject_type.get_type_desciption(), arm.case_name));
            return;
        }

        if (covered[entry->ordinal]) {
            refuse(node, arm.token, fmt::format(
                "'{}' is already covered by an earlier arm.", arm.case_name));
            return;
        }

        covered[entry->ordinal] = true;
        arm.case_ordinal = entry->ordinal;

        // the bindings, which the arm's scope holds as its own first declarations. **the count is
        // checked against the case rather than against the constructor**: a pattern is not a call, and
        // asking the overload set would make "how many names may I write here" depend on a ranking
        const size_t bound = arm.scope != nullptr ? arm.scope->children.size() : 0;
        std::vector<VarDeclNode *> bindings;

        for (size_t i = 0; i < bound; i++) {
            // the bindings are the scope's *leading* children, so the first thing that is not one ends
            // the run - a block arm's own statements follow them. asked through has_type rather than
            // through a null get_ptr, which asserts on a mismatch rather than answering
            if (!arm.scope->children[i].has_type<VarDeclNode>()) {
                break;
            }

            bindings.push_back(arm.scope->children[i].get_ptr<VarDeclNode>());
        }

        if (bindings.size() != entry->payload_field_count) {
            refuse(node, arm.token, fmt::format(
                "'{}' carries {} value{}, so this pattern binds {} name{} - {} written.",
                arm.case_name,
                entry->payload_field_count,
                entry->payload_field_count == 1 ? "" : "s",
                entry->payload_field_count,
                entry->payload_field_count == 1 ? "" : "s",
                bindings.size()));
            return;
        }

        for (size_t i = 0; i < bindings.size(); i++) {
            VarDeclNode *binding = bindings[i];

            // the parser seats a `unknown&` placeholder so a member call in the arm knows the
            // receiver is already an address - so "has it been given its real type" is a question
            // about the *pointee*, not about whether a type node is there at all
            if (binding->has_type() && !value_type_of(binding->type()).is_unknown()) {
                continue;  // already seated on an earlier round
            }

            const ComplexType::Property &prop =
                ct->get_property(entry->first_payload_property + i);

            // **`V& $v = &$__match->__cN_field`** - a *borrow* of the payload, not a copy of it, and
            // that is the one decision in this pass that is not bookkeeping.
            //
            // a copy would be dropped at the end of the arm's scope, and an arm's *value* is evaluated
            // after that scope - so a `string` binding would be freed one instruction before the value
            // that reads it. a borrow owns nothing, so AST::OwnershipPass emits no drop and the
            // ordering question does not arise. it is also simply right: the subject is a local this
            // node owns and it outlives every arm, so there is nothing here for a copy to protect
            // against, and a `match` over an owning enum now costs no allocation at all
            //
            // the same shape a `foreach` element and `contract::unwrappable<V>::unwrap()` already have,
            // which is why reading one needs nothing new: AST::PointerAdjuster wraps the read, the
            // binding reads as `V`, and a use that wants a value of its own copies at the arrival
            ExprNode *member = make_member_place(*_current_module, *subject, prop.name, arm.token);

            binding->init_expr = &_current_module->nodes.emplace_back<AddrOfExprNode>(member);

            auto &type_node = _current_module->nodes.emplace_back<TypeNode>(
                ValueType::make_pointer(prop.type, false));
            binding->set_type_node(&type_node);

            _changed = true;
        }
    }

    // **exhaustiveness, and it is what makes a match worth having.** an enum's case list is closed, so
    // "every case is named" is a question with an answer - and adding a case to an enum then breaking
    // every match over it is the report a reader wants, not a silent fallthrough
    std::vector<std::string> missing;

    for (const ComplexType::EnumCase &entry : ct->enum_cases()) {
        if (!covered[entry.ordinal]) {
            missing.push_back(entry.name);
        }
    }

    if (!has_else && !missing.empty()) {
        std::string list = missing[0];

        for (size_t i = 1; i < missing.size(); i++) {
            list += ", " + missing[i];
        }

        refuse(node, node.token, fmt::format(
            "this 'match' does not cover every case of '{}' - {} left out. Name {}, or add an "
            "'else' arm.",
            subject_type.get_type_desciption(),
            missing.size() == 1 ? "one is" : fmt::format("{} are", missing.size()),
            list));
        return;
    }

    // an `else` over a set that is already covered is dead code, and dead code in a match is usually a
    // case that was renamed. reported rather than dropped, since which of the two the author meant is
    // not this pass's to guess
    if (has_else && missing.empty() && !ct->enum_cases().empty()) {
        refuse(node, node.token, fmt::format(
            "this 'match' already covers every case of '{}', so its 'else' arm can never run.",
            subject_type.get_type_desciption()));
        return;
    }

    // **the arms have to meet at one type**, and a `{ }` arm meets at `void`. one answer for the whole
    // form rather than one per arm: the value a match hands back is a single value, so an arm list that
    // disagreed would be a phi with two types
    ValueType unified = ValueType::make_void();
    bool first = true;

    for (MatchExprNode::Arm &arm : node.arms) {
        // **read through one pointer level**, which is the auto-deref AST::PointerAdjuster performs -
        // and it has not run yet, this pass being inside the fixpoint that precedes it. so a binding
        // read answers `int32&` here and `int32` at codegen, and unifying against the unpeeled answer
        // would make `=> $v` and `=> $v * 1000` two different types for no reason a reader could see.
        //
        // AST::value_type_of is that one rule and exactly one level, so this is not compensating for
        // the adjuster - it is asking the same question the adjuster answers, at the only moment this
        // pass can ask it
        const ValueType arm_type = arm.value != nullptr
            ? value_type_of(arm.value->result_type())
            : ValueType::make_void();

        if (is_undetermined_type(arm_type) && arm.value != nullptr) {
            if (_finalizing) {
                refuse(node, arm.token, "this arm's value has no settled type.");
            }

            return;
        }

        if (first) {
            unified = arm_type;
            first = false;
            continue;
        }

        if (unified == arm_type) {
            continue;
        }

        // the wider of the two, through the same rule every other destination uses. a `void` beside a
        // value is not a widening in either direction, which is what makes mixing the two shapes the
        // located error it should be rather than a phi of one arm
        if (is_implicitly_convertible(arm_type, unified)) {
            continue;
        }

        if (is_implicitly_convertible(unified, arm_type)) {
            unified = arm_type;
            continue;
        }

        refuse(node, arm.token, fmt::format(
            "the arms of this 'match' have to produce one type - this one is '{}' where an earlier one "
            "is '{}'.{}",
            arm_type.get_type_desciption(),
            unified.get_type_desciption(),
            (arm_type.is_void() || unified.is_void())
                ? " An arm written '{ ... }' produces nothing, so every arm has to be one."
                : ""));

        return;
    }

    node.result = unified;
    node.patterns_decided = true;
    _changed = true;
}

};
