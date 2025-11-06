#include "AST/ASTOperatorRewriter.h"

#include "AST/ASTArrayLiteral.h"
#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTDetach.h"
#include "AST/ASTIssue.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTNullability.h"
#include "AST/GuardNode.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/OperatorNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeCastNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/TypeNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/LoopControlNode.h"
#include "AST/ForeachNode.h"
#include "AST/TemporaryBindExprNode.h"

#include <fmt/core.h>

namespace AST
{

OperatorRewriter::OperatorRewriter(Bundle &bundle)
    : _bundle(bundle), _collector(bundle.collector)
{
}

CodeRef OperatorRewriter::code_ref_for(const TokenReference &token)
{
    return CodeRef{_current_module, _current_file, token.make_slice()};
}

bool OperatorRewriter::run_round()
{
    _changed = false;

    for (auto &module_ptr : _bundle.modules) {
        _current_module = module_ptr.get();

        for (auto &file : module_ptr->files()) {
            _current_file = &file;

            // per file, so a case's hoist names do not move when an unrelated file above it grows a
            // literal - the locality OwnershipPass::_temporary_count has per body. a round that hoists
            // nothing new leaves the numbering exactly where the round that did put it, because a
            // decided literal is never hoisted twice
            _hoist_count = 0;

            if (file.root != nullptr) {
                file.root->accept(*this);
            }
        }
    }

    // **once for the round, not once per discard** - see _detached. nothing between a rewrite and here
    // reads NodeCollection::of_type: this walk goes through scope children, and the sweeps that do care -
    // Monomorphizer::snapshot_calls and TypeLowering::build_function_maps - are the next round and
    // codegen respectively. finalize() calls this, so the flush covers that pass too
    _detached.flush(_bundle);

    return _changed;
}

void OperatorRewriter::finalize()
{
    // one more round, rather than a sweep over `nodes.of_type<ArrayLiteralExprNode>()`: a round
    // inherits visitFunctionDecl's generic-body skip, so a literal inside a template nothing
    // instantiated is not blamed for a destination that was never going to be substituted - and it
    // sees only literals a scope still holds, where a sweep would see detached ones forever
    _finalizing = true;
    run_round();
    _finalizing = false;
}

FunctionCallExprNode &OperatorRewriter::build_call(
    const std::string &name,
    const TokenReference &at,
    std::vector<ExprNode *> operands,
    const Namespace *lookup)
{
    const TokenReference name_token =
        _current_module->make_virtual_token(name, Token::Type::t_identifier, at);

    auto &call = _current_module->nodes.emplace_back<FunctionCallExprNode>(
        name_token, std::move(operands));

    call.lookup_namespace = lookup;

    // left unresolved on purpose. the fixpoint's own settle_calls is what finishes every other
    // pending call, and a call built here may name a *generic* declaration that the next round still
    // has to instantiate - which is the whole reason this pass runs inside the fixpoint
    _changed = true;

    return call;
}

FunctionCallExprNode &OperatorRewriter::build_operator_call(
    const std::string &spelling,
    OpFixity fixity,
    const TokenReference &at,
    std::vector<ExprNode *> operands)
{
    auto &call = build_operator_call_node(
        *_current_module, _collector, spelling, fixity, at, std::move(operands));

    _changed = true;

    return call;
}

void OperatorRewriter::widen_binary_operands(BinaryExprNode &bin)
{
    // **the post-parse moment of the parser's binary reconciliation** - the rule itself is
    // AST::common_numeric_type's, shared with Parser::parse_binary_expr, and only the insertion is here.
    //
    // the parser can only reconcile operands whose type it knows *then*. a declaration typed by a later
    // pass misses it entirely - `foreach ($a as $i => $x) { if ($i == 0) ... }` is the shape that made
    // this reachable, since `$i` has no type until AST::ForeachLowering reads the key contract off the
    // cursor, by which point the literal has long since defaulted to int32.
    //
    // the failure mode was codegen asserting on `Both operands to ICmp instruction are not of the same
    // type`, which names neither the loop nor the comparison. this pass is the right owner of the second
    // moment: its header states it exists for "operand syntax whose meaning depends on a type this
    // fixpoint is still deciding", and a binary expression whose operands only now have types is that
    if (bin.lhs == nullptr || bin.rhs == nullptr) {
        return;
    }

    // **an address is never a width to reconcile.** pointer arithmetic and pointer comparison each have
    // their own arm in BinaryExprNode::result_type(), and a cast inserted here would convert the address
    // itself to an integer - which is what `string::view($this->bytes:$ + $from, $len)` turns into if this
    // exits any later. Asked of the *raw* operand types, because this pass runs ahead of
    // AST::PointerAdjuster and a borrow still reads as a pointer here
    const ValueType raw_left = bin.lhs->result_type();
    const ValueType raw_right = bin.rhs->result_type();

    if (raw_left.is_pointer() || raw_right.is_pointer()) {
        return;
    }

    // asked before the operand types, because it is the cheap half and it is false for all but a
    // handful of nodes: already reconciled - by the parser, or by an earlier round of this.
    //
    // a void answer is what "not reconciled" looks like from the outside; anything else is either not
    // ready yet or not this function's business, since a class and a nullable both have their own arms
    // in BinaryExprNode::result_type() and never reach a void answer through width alone
    //
    // **a comparison is the one shape that answer cannot speak for**, and it has to be asked separately:
    // it is a `bool` whatever it compared, so a `usize` against an `int32` literal looks perfectly
    // reconciled from the outside while codegen still gets two widths. That is the very case the header
    // above describes - `$i == 0` over a `foreach` key - and it reached codegen's
    // "Both operands to ICmp instruction are not of the same type" for as long as the gate was the
    // result type alone
    const bool is_comparison = bin.op_node != nullptr && bin.op_node->op != nullptr
        && bin.op_node->op->is_comparison();

    if (!is_comparison && !bin.result_type().is_void()) {
        return;
    }

    const ValueType left = value_type_of(raw_left);
    const ValueType right = value_type_of(raw_right);

    const auto common = common_numeric_type(left, right);

    if (!common.has_value()) {
        return;
    }

    // exactly one side differs: the common type is always one of the two
    if (left.get_primitive_type() != common->get_primitive_type()) {
        bin.lhs = &_current_module->nodes.emplace_back<TypeCastNode>(*common, bin.lhs, true);
    }
    else {
        bin.rhs = &_current_module->nodes.emplace_back<TypeCastNode>(*common, bin.rhs, true);
    }

    _changed = true;
}

void OperatorRewriter::resolve_index(IndexExprNode &index_expr)
{
    if (index_expr.resolution_decided || index_expr.base == nullptr) {
        return;
    }

    // the node owns this question - see AST::IndexExprNode::indexed_base_type, whose other
    // asker is result_type(). one answer, so "is this a pointer index" cannot be decided twice
    const ValueType base_type = index_expr.indexed_base_type();

    // nothing to decide yet. a type parameter is substituted by a later round, and an unsettled call
    // answers void - both are "ask again", not "wrong"
    if (is_undetermined_type(base_type)) {
        return;
    }

    index_expr.resolution_decided = true;

    // **`$a[]` cannot be read.** it names the slot after the last one, and nothing has put a value
    // there - so a read would hand back whatever the buffer happened to hold. the two positions that
    // *bind* the slot instead of reading it are legal, and the parser recorded which this is
    if (index_expr.is_append() && !index_expr.slot_is_bound) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(index_expr.token_bracket),
            "'$a[]' names the slot after the last one, so there is nothing there to read. Write to "
            "it ('$a[] = ...'), borrow it ('&$a[]'), or index an existing element.");
        return;
    }

    // **a raw pointer, and the one spelling of it is `$p:$[n]`.** every operation on an address goes
    // through `:$` - comparison, arithmetic, casting - and indexing was the one hole in that rule
    // (todo/B9). closing it is what lets a bare `[` mean exactly one thing: ask the container
    if (base_type.is_pointer()) {
        if (!index_expr.base_was_peeled) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(index_expr.token_bracket),
                fmt::format(
                    "'{}' is a pointer, and a pointer is indexed through ':$' - write '$p:$[...]'. "
                    "An unqualified '[' asks a collection for one of its elements.",
                    base_type.get_type_desciption()));
            return;
        }

        if (index_expr.indices.size() != 1) {
            _collector.collect_issue<Issue::GenericError>(
                code_ref_for(index_expr.token_bracket),
                fmt::format(
                    "a pointer takes exactly one index, but {} were written. Several indices are an "
                    "element contract a collection declares, not address arithmetic.",
                    index_expr.indices.size()));
            return;
        }

        // decided, and the existing GEP arm in the codegen is the answer. nothing to rewrite
        return;
    }

    if (!base_type.has_complex_type()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(index_expr.token_bracket),
            fmt::format("'{}' cannot be indexed.", base_type.get_type_desciption()));
        return;
    }

    // **the guard rail behind resolve_index_write.** that rewrite can only run from a scope's child list,
    // because AST::RecursiveVisitor::statement_edge descends into a statement and never replaces one - so
    // it depends on every AssignNode being a scope child, which every producer does satisfy today.
    //
    // where that stops being true this arm is what says so. reaching here with an `=` behind the bracket
    // and a declared write contract means the rewrite never got its turn, and building the read call would
    // compile the program into an insert that *asserts* instead - the exact shape of silent failure
    // CLAUDE.md's "a transient node owes two arms" trap describes. reported as a compiler bug, the way
    // TypeChecker::visit_addr_of_expr reports a temporary nothing gave a slot
    if (index_expr.is_assignment_target
        && declares_index_write(_collector, base_type, index_expr.indices.size())) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(index_expr.token_bracket),
            fmt::format(
                "compiler bug: '{}' declares an element-write contract and this bracket has an '=' behind "
                "it, but the write was never rewritten - the assignment is not a scope's statement. See "
                "AST::OperatorRewriter::resolve_index_write.",
                base_type.get_type_desciption()));
        return;
    }

    // asked of the one owner, the mirror of index_write_operator_name() above - so "which name does a
    // bracket register under" has one answer per contract rather than one per asker
    const std::string &decorated_name = index_operator_name();

    // **is there an element contract at all?** asked ahead of building the call, because "this type
    // has none" and "none of the ones it has fits" are different things to say and only the first is
    // answerable without an overload set. the second is the ordinary NoMatchingOverload the
    // finalizing sweep reports, in the operator's own words
    const auto candidates =
        _collector.functions.overloads(decorated_name, _collector.namespaces.root());

    if (candidates.empty()) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(index_expr.token_bracket),
            fmt::format(
                "'{}' has no element contract, so it cannot be indexed. Declare one, e.g. "
                "'operator ({}& $c)[usize $i] : ...'.",
                base_type.get_type_desciption(), base_type.get_type_desciption()));
        return;
    }

    // the container is the first operand and the indices follow, which is the order the declaration
    // writes them in: `operator (array<T>& $a)[usize $i]`. the receiver is *not* addressed here - the
    // parameter is a borrow, and AST::CallResolver inserts the address-of a borrow parameter wants,
    // exactly as it does for every other call
    std::vector<ExprNode *> operands;
    operands.reserve(index_expr.indices.size() + 1);
    operands.push_back(index_expr.base);
    for (auto *index : index_expr.indices) {
        operands.push_back(index);
    }

    index_expr.element_call = &build_operator_call(
        OperatorRegistry::bracket_spelling(), OpFixity::t_index, index_expr.token_bracket,
        std::move(operands));

    // **the operands now belong to the call.** left here as well they would have two parents, and
    // AST::PointerAdjuster rewrites edges in place - so each one would collect a second deref
    index_expr.base = nullptr;
    index_expr.indices.clear();
}

void OperatorRewriter::resolve_index_write(ScopeNode &scope, size_t index)
{
    Node *statement = scope.children[index].node();

    if (statement == nullptr || statement->get_node_type() != NodeType::n_assign) {
        return;
    }

    auto *assign = static_cast<AssignNode *>(statement);

    if (assign->target == nullptr
        || assign->target->get_node_type() != NodeType::n_expr_index
        || assign->value_expr == nullptr) {
        return;
    }

    auto *bracket = static_cast<IndexExprNode *>(assign->target);

    // already decided - as a read by resolve_index, or as a write by an earlier round
    if (bracket->resolution_decided || bracket->base == nullptr) {
        return;
    }

    // the node owns this question, and asking it here is the *same* ask resolve_index makes one step
    // later - see the header for why the two must not be split across a round
    const ValueType base_type = bracket->indexed_base_type();

    // nothing to decide yet: a type parameter a later round substitutes, an unsettled call. deliberately
    // left unmarked, so resolve_index does not decide either and body_is_concrete keeps saying no
    if (is_undetermined_type(base_type)) {
        return;
    }

    // a pointer index and an unindexable type are resolve_index's arms, and its wordings. a second
    // answer here would be a second message for one refusal
    if (base_type.is_pointer() || !base_type.has_complex_type()) {
        return;
    }

    if (!declares_index_write(_collector, base_type, bracket->indices.size())) {
        return;
    }

    // **a `const` container has no write contract to reach**, and this is where that is said. left to the
    // call, it would come back as "no overload of 'operator []=' accepts these arguments" with a candidate
    // list - true, and useless beside the message every other const write gets. the wording is
    // TypeChecker::check_const_target's, because it is the same refusal one pass earlier
    if (base_type.is_const()) {
        _collector.collect_issue<Issue::ConstViolation>(
            code_ref_for(assign->token_assign),
            fmt::format(
                "cannot write to an element of '{}' - it is const, and the element contract that writes "
                "takes the container as a mutable borrow",
                base_type.get_type_desciption()));

        bracket->resolution_decided = true;
        return;
    }

    // **the container has to have storage the write can reach.** as an assignment target this was
    // AST::OwnershipPass's MaterializationScope refusal; as a call operand a temporary would merely be
    // bound for the statement, and the write would land in a container destroyed at the end of it.
    //
    // asked through the predicate OwnershipPass and TypeChecker already share, so there is no second
    // reading of "does this operand need storage minted for it". deliberately *not*
    // `!is_place_expression`: a call returning `map<K, V>&` is materializable too, and
    // `$maps->at(0)[$k] = $v` writes through a borrow into storage somebody else owns
    if (borrow_operand_needs_storage(*bracket->base)) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(bracket->token_bracket),
            fmt::format(
                "'{}' has no storage of its own, so writing to one of its elements would be lost - the "
                "value is destroyed at the end of this statement. Bind it to a variable first.",
                base_type.get_type_desciption()));

        bracket->resolution_decided = true;
        return;
    }

    // the container, then the indices, then the value - the order the declaration writes its three
    // operand lists in. the receiver is *not* addressed here: the parameter is a borrow, and
    // AST::CallResolver inserts the address-of a borrow parameter wants, exactly as it does for the
    // borrowing bracket and for every other call
    std::vector<ExprNode *> operands;
    operands.reserve(bracket->indices.size() + 2);
    operands.push_back(bracket->base);
    for (auto *bracket_index : bracket->indices) {
        operands.push_back(bracket_index);
    }
    operands.push_back(assign->value_expr);

    auto &call = build_operator_call(
        OperatorRegistry::bracket_spelling(), OpFixity::t_index_write, bracket->token_bracket,
        std::move(operands));

    // **release what is kept, before collecting what is going.** every operand belongs to the call now,
    // and AST::forget_subtree's collector is total by construction - so an operand still hanging off the
    // assignment or the bracket would be forgotten with them, and forgetting a node the tree still holds
    // is a symbol nothing declares. AST::ConstFolding::splice nulls the arm it keeps for the same reason
    bracket->base = nullptr;
    bracket->indices.clear();
    assign->value_expr = nullptr;

    // and decided, so nothing asks again about a node that is leaving
    bracket->resolution_decided = true;

    // **the old value's teardown cannot have been decided yet**, and the reason is the round order rather
    // than luck: body_is_concrete answers false while this bracket is undecided, so AST::OwnershipPass
    // has not walked this body. asserted rather than commented, because forgetting a `teardown_old` scope
    // would silently un-emit the destructor calls inside it
    assert(assign->teardown_old == nullptr && !assign->releases_old
        && "an index write is rewritten before ownership has walked the body");

    // `target` deliberately still points at the bracket: the bracket is what is going away, and the
    // collecting walk has to reach it
    _detached.collect(*assign);

    scope.children[index] = make_ref(call);
}

void OperatorRewriter::report_unplaced_literal(ArrayLiteralExprNode &literal)
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

OperatorRewriter::LiteralDestination OperatorRewriter::literal_destination(Node *statement)
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

    // an assignment's right-hand side, and **only over a plain variable**: `$a = [1, 2, 3];`. the
    // expansion needs one fresh place per append, and a variable is what can be respelled from its
    // declaration. any other target - `$s->items = [...]` - would need the target subtree cloned per
    // element, which is A13b's temporary problem wearing different clothes
    if (statement->get_node_type() == NodeType::n_assign) {
        auto *assign = static_cast<AssignNode *>(statement);
        auto *literal = array_literal_of(assign->value_expr);

        if (literal == nullptr) {
            return {};
        }

        if (assign->target == nullptr || assign->target->get_node_type() != NodeType::n_varref) {
            return {nullptr, literal, nullptr};
        }

        return {place_root_of(assign->target), literal, &assign->value_expr};
    }

    return {};
}

bool OperatorRewriter::settle_destination_type(
    const LiteralDestination &destination, ValueType &settled)
{
    ArrayLiteralExprNode &literal = *destination.literal;

    ValueType destination_type =
        destination.decl->has_type() ? destination.decl->type() : ValueType::make_unknown();

    // **the declaration said nothing about what holds these, so the elements are asked.**
    // AST::array_literal_type_for owns that question and answers three ways, so `[f(), g()]` is asked
    // again next round rather than refused on the first - `$a = [1, 2, 3]` is an `array<int32>`
    // because its elements say so, which is what book/concept/arrays.md specifies
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
            // AST::infer_declaration_type applies at the two other moments, and the half its own
            // comment records getting dropped once already
            destination_type = infer_declaration_type(look.type, destination_type.is_const());

            destination.decl->set_type_node(
                &_current_module->nodes.emplace_back<TypeNode>(destination_type));

            _changed = true;
        }
    }

    // not decided yet: the declaration may be typed from a call this fixpoint has not settled, or an
    // element above may be. **out of rounds is out of answers** - see finalize()
    if (is_undetermined_type(destination_type)) {
        // the message is only for the case where nothing else explained it, which
        // has_critical_issues() is already the compiler's answer to - deciding it either way is what
        // keeps a literal from reaching codegen unexpanded
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

void OperatorRewriter::expand_array_literal(ScopeNode &scope, size_t index)
{
    const LiteralDestination destination = literal_destination(scope.children[index].node());

    if (destination.literal == nullptr || destination.literal->expansion_decided) {
        return;
    }

    ArrayLiteralExprNode &literal = *destination.literal;

    if (destination.decl == nullptr) {
        report_unplaced_literal(literal);
        return;
    }

    ValueType destination_type;

    if (!settle_destination_type(destination, destination_type)) {
        return;
    }

    literal.expansion_decided = true;

    std::vector<NodeReference> appends;

    if (!build_literal_expansion(
            literal, *destination.decl, destination_type, destination.slot, appends)) {
        return;
    }

    scope.children.insert(scope.children.begin() + index + 1, appends.begin(), appends.end());

    _changed = true;
}

bool OperatorRewriter::build_literal_expansion(
    ArrayLiteralExprNode &literal,
    VarDeclNode &into,
    const ValueType &type,
    ExprNode **slot,
    std::vector<NodeReference> &appends)
{
    // has_property_layout, not has_complex_type: the expansion below asks the destination for a
    // zero-argument constructor and appends into the places it makes, and an interface declares
    // neither. so an array literal assigned to an interface-typed destination is the "cannot be built
    // from a literal" diagnostic below rather than a lookup that finds nothing
    ComplexType *ct = type.has_property_layout() ? type.get_complex_type() : nullptr;

    if (ct == nullptr) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(literal.token_bracket),
            fmt::format(
                "'{}' cannot be built from an array literal - a literal fills a collection, through "
                "its constructor and its append operator.",
                type.get_type_desciption()));
        return false;
    }

    // **the constructor of the destination type, named rather than looked up.** an instantiation
    // carries its template's name and its own type arguments, which is exactly what a call site
    // writes as `array<int32>()` - so this builds the call a user would have written and lets
    // AST::CallResolver choose the overload, with no rule of its own about what a collection is
    const ComplexType *tmpl = ct->template_or_self();

    std::vector<TypeNode *> type_args;
    for (const auto &arg : ct->instantiation_args) {
        type_args.push_back(&_current_module->nodes.emplace_back<TypeNode>(arg));
    }

    auto &ctor = build_call(
        tmpl->name.value_or(std::string()), literal.token_bracket, {},
        tmpl->ast_namespace != nullptr ? tmpl->ast_namespace : &_collector.namespaces.root());

    ctor.explicit_type_args = std::move(type_args);

    *slot = &ctor;

    // one `$dest[] = element` per element. each gets its **own** place naming the destination - one
    // subtree per append, because PointerAdjuster rewrites edges in place and a shared one would be
    // adjusted once per use
    appends.reserve(literal.elements.size());

    for (auto *element : literal.elements) {
        auto &var = _current_module->nodes.emplace_back<VarNode>(&into);
        auto &var_ref = _current_module->nodes.emplace_back<VarRefNode>(&var);

        auto &slot_expr = _current_module->nodes.emplace_back<IndexExprNode>(
            &var_ref, std::vector<ExprNode *>{}, literal.token_bracket);

        // the two facts the parser records for a hand-written `$a[] = v`, recorded here for the same
        // reasons: the slot is bound rather than read, and the write into it initializes storage
        // that holds nothing, so no teardown is owed
        slot_expr.slot_is_bound = true;

        auto &assign = _current_module->nodes.emplace_back<AssignNode>(
            &slot_expr, element, literal.token_bracket);
        assign.is_initialization = true;

        appends.push_back(make_ref(assign));
    }

    return true;
}

ExprNode *OperatorRewriter::hoist_array_literal(ArrayLiteralExprNode &literal)
{
    // under a `?->` or a `??`, where hoisting would move the construction above the branch that
    // decides whether it happens at all. refused outright rather than waited on - no later round makes
    // it safe - so report_unplaced_literal is the answer, and its message is already the right one:
    // the literal has to name its storage, which here means the author writing the declaration
    if (_hoist_barrier > 0) {
        report_unplaced_literal(literal);
        return nullptr;
    }

    // no destination has spoken yet. AST::CallResolver types an argument's literal when it settles the
    // call, which is a later step of this same round - so this is the ordinary "ask again next round",
    // and finalize() is what makes being out of rounds a refusal
    if (!literal.bound_type.has_value()) {
        return nullptr;
    }

    literal.expansion_decided = true;

    // minted after parsing, so no lexical namespace and no block token - nothing reads
    // ScopeNode::lookup_variable past the parser. the name is numbered for the reason a temporary's
    // is: a statement may hold two, and a dump in which both read `$__lit` cannot be asserted about
    auto &decl = _current_module->nodes.emplace_back<VarDeclNode>(
        _current_module->make_virtual_token(
            fmt::format("$__lit{}", ++_hoist_count), Token::Type::t_varname, literal.token_bracket),
        &_current_module->nodes.emplace_back<TypeNode>(*literal.bound_type));

    std::vector<NodeReference> appends;

    if (!build_literal_expansion(literal, decl, *literal.bound_type, &decl.init_expr, appends)) {
        return nullptr;
    }

    // the declaration first, then its appends: the statement being walked comes after all of them,
    // and visitScope wraps the lot in a scope of its own
    _hoisted.push_back(make_ref(decl));
    _hoisted.insert(_hoisted.end(), appends.begin(), appends.end());

    _changed = true;

    auto &var = _current_module->nodes.emplace_back<VarNode>(&decl);

    return &_current_module->nodes.emplace_back<VarRefNode>(&var);
}

ExprNode *OperatorRewriter::rewrite_value_edge(ExprNode *expr)
{
    if (expr == nullptr) {
        return nullptr;
    }

    // an array literal reaching here is one no *statement* claimed - the vardecl and assign arms hand
    // theirs to expand_array_literal above and do not descend into them. so this is a literal in an
    // expression position: an argument, which a destination can type, or a position nothing will ever
    // type, which report_unplaced_literal is still for
    //
    // the hoist answers null while it is waiting, and the literal is then left exactly as written -
    // the three-state shape expansion_decided exists for. only finalize() turns waiting into a refusal
    if (auto *literal = array_literal_of(expr)) {
        if (literal->expansion_decided) {
            return expr;
        }

        if (ExprNode *hoisted = hoist_array_literal(*literal)) {
            return hoisted;
        }

        if (_finalizing) {
            report_unplaced_literal(*literal);
        }

        return expr;
    }

    expr->accept(*this);

    return resolve_builtin_operator(expr);
}

ExprNode *OperatorRewriter::resolve_builtin_operator(ExprNode *expr)
{
    // **the parser's own decision, re-asked.** `parse_binary_expr` chooses between a declared
    // operator and the built-in arms by asking AST::binary_has_builtin_meaning, and in a template
    // body the operands are a bare `T` - which that predicate is deliberately non-committal about,
    // so the built-in path wins and a BinaryExprNode is what the body keeps. cloning does not
    // re-parse, so the substituted body still holds it (todo/A32).
    //
    // the same predicate and the same operand normalizer, so this is a second *moment*, not a second
    // rule. a node that legitimately has a built-in meaning answers true here exactly as it did at
    // parse time and is left alone, which is what makes this self-guarding: the only nodes it
    // rewrites are the ones the parser could not have
    if (expr->get_node_type() == NodeType::n_expr_binary) {
        auto *bin = static_cast<BinaryExprNode *>(expr);

        if (bin->op_node == nullptr || bin->op_node->op == nullptr
            || !bin->op_node->op->has_fixity(OpFixity::t_infix)
            || binary_has_builtin_meaning(
                bin->op_node->op, parse_time_operand(bin->lhs), parse_time_operand(bin->rhs))) {
            return expr;
        }

        return &build_operator_call(
            bin->op_node->op->spelling, OpFixity::t_infix, bin->op_node->token_literal,
            {bin->lhs, bin->rhs});
    }

    if (expr->get_node_type() == NodeType::n_expr_unary) {
        auto *un = static_cast<UnaryExprNode *>(expr);

        // a unary node keeps its token rather than an OperatorNode, so the symbol is looked up the
        // way Parser::parse_prefix_unary looks it up - one registry, one answer
        const Operator *op = _collector.operators.get_operator(un->token_operator);

        if (op == nullptr || !op->has_fixity(OpFixity::t_prefix)
            || unary_has_builtin_meaning(op, parse_time_operand(un->expr))) {
            return expr;
        }

        return &build_operator_call(
            op->spelling, OpFixity::t_prefix, un->token_operator, {un->expr});
    }

    return expr;
}

// ---- the arms that are not the base's default -------------------------------------------------

void OperatorRewriter::visitScope(ScopeNode &node)
{
    // the base already walks children by index, but the literal expansion has to run *before* the
    // child is rewritten - so the constructor and the appends it produces are walked by this same
    // pass rather than waiting a round. that ordering is this pass's, so the loop is too
    for (size_t i = 0; i < node.children.size(); i++) {
        // **ahead of both the literal expansion and the descent**, and see resolve_index_write for why
        // each of those orderings is the content rather than a preference: after the descent the bracket
        // has already been decided a read, and after the expansion a literal right-hand side is reported
        // against a destination this rewrite was about to remove
        resolve_index_write(node, i);

        expand_array_literal(node, i);

        // **saved and restored around the descent**, because a nested block runs this very loop: a
        // literal hoisted inside it belongs to *its* statement, and an inner scope finishing would
        // otherwise hand its leftovers to whatever statement this level was in the middle of
        std::vector<NodeReference> outer;
        outer.swap(_hoisted);

        statement_edge(node.children[i].node());

        if (!_hoisted.empty()) {
            wrap_statement_with_hoists(node, i);
        }

        _hoisted.swap(outer);
    }
}

void OperatorRewriter::wrap_statement_with_hoists(ScopeNode &scope, size_t index)
{
    // **a scope of its own, rather than the declarations spliced into this one.** the difference is
    // the lifetime: a local of the enclosing frame is dropped when *that* frame ends, so
    // `f([1, 2, 3])` in a loop body would hold one live collection per iteration until the whole
    // block ended. wrapped, the frame is the statement's, and AST::OwnershipPass's ordinary
    // reverse-order scope drop destroys it where the statement finishes - no new mechanism, and a
    // `return` or a `break` inside the statement unwinds through it like any other frame
    //
    // the same shape AST::ForeachLowering's wrapper has, for the same reason, down to carrying no
    // block token and no lexical namespace
    auto &wrapper = _current_module->nodes.emplace_back<ScopeNode>();
    wrapper.parent_ptr = &scope;

    wrapper.children = std::move(_hoisted);
    wrapper.children.push_back(scope.children[index]);

    _hoisted.clear();

    scope.children[index] = make_ref(wrapper);
}

void OperatorRewriter::visitFunctionDecl(FunctionDeclNode &node)
{
    // a template's body is only meaningful once cloned into a concrete instance, and its operand
    // types are the very things that are not known there. PointerAdjuster's rule
    if (node.is_generic()) {
        return;
    }

    RecursiveVisitor::visitFunctionDecl(node);
}

void OperatorRewriter::visitVarDecl(VarDeclNode &node)
{
    // an array literal initializer belongs to the enclosing scope, which has already had its turn at
    // it above - descending here would report it as an expression in a position that cannot hold one
    if (array_literal_of(node.init_expr) != nullptr) {
        return;
    }

    RecursiveVisitor::visitVarDecl(node);
}

void OperatorRewriter::visit_assign(AssignNode &node)
{
    value_edge(node.target);

    // as for a declaration above: the scope owns an array literal on the right-hand side
    if (array_literal_of(node.value_expr) == nullptr) {
        value_edge(node.value_expr);
    }

    statement_edge(node.teardown_old);
}

void OperatorRewriter::visitBinaryExpr(BinaryExprNode &node)
{
    RecursiveVisitor::visitBinaryExpr(node);
    widen_binary_operands(node);
}

ExprNode *OperatorRewriter::upgrade_optional_operand(ExprNode *operand, const TokenReference &at)
{
    ExprNode *upgraded = optional_operand_of(operand, *_current_module, at);

    if (upgraded != operand) {
        _changed = true;
    }

    return upgraded;
}

void OperatorRewriter::visit_guard(GuardNode &node)
{
    RecursiveVisitor::visit_guard(node);

    // the tested value is the declaration's own initializer - a guard has no separate condition edge
    if (node.decl != nullptr) {
        node.decl->init_expr = upgrade_optional_operand(node.decl->init_expr, node.token);
    }
}

void OperatorRewriter::visit_null_coalesce(NullCoalesceExprNode &node)
{
    // the other half of the rule above: `$a ?? [1, 2]` evaluates its right side only when the left was
    // null, and a hoist would build the collection either way
    const HoistBarrier barrier(*this);

    RecursiveVisitor::visit_null_coalesce(node);

    node.lhs = upgrade_optional_operand(node.lhs, node.token);
}

void OperatorRewriter::visit_optional_chain(OptionalChainExprNode &node)
{
    // **the one position a hoist must not reach.** everything under a `?->` runs only when the base
    // was there, and a hoisted declaration runs unconditionally, ahead of the statement - so
    // `$o?->take([f()])` would call `f()` on the null path too. `?->` and `??` are the only two forms
    // in the language that do not evaluate their right side, which is what makes this a two-arm rule
    // rather than a control-flow analysis
    const HoistBarrier barrier(*this);

    RecursiveVisitor::visit_optional_chain(node);

    ExprNode *base = upgrade_optional_operand(node.base, node.token);

    if (base == node.base) {
        return;
    }

    node.base = base;

    // **and the marker's stored type, which nothing else can repair.** a ChainBaseNode holds the
    // base's non-null type rather than an edge to the base, and clone only *substitutes* it - so a
    // marker minted for a bare `T` becomes `weak<Node>` where it should have become `Node`. re-derived
    // here, at the one moment the base is known to have changed shape (todo/A35 is why it cannot be
    // re-derived from the marker's own side)
    if (node.chain_base != nullptr) {
        node.chain_base->type = unwrapped_type_of(node.base->result_type());
    }
}

void OperatorRewriter::visit_index_expr(IndexExprNode &node)
{
    // **the operands before the bracket itself**: `$a[$b[0]]` has to decide the inner one before the
    // outer can read a type off it, which is what descending first gets. the base's own order is not
    // a second rule to keep in step with - `element_call` and the operands are never both present
    RecursiveVisitor::visit_index_expr(node);

    resolve_index(node);
}

};
