#include "AST/ASTOperatorRewriter.h"

#include "AST/ASTArrayLiteral.h"
#include "AST/ASTArrayLiteralExpansion.h"
#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTDetach.h"
#include "AST/ASTIssue.h"
#include "AST/ASTLiteralTyping.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTNullability.h"
#include "AST/GuardNode.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTVariadic.h"
#include "AST/ASTPlaceExpr.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/IfStatementNode.h"
#include "AST/LiteralValueNode.h"
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
    return CodeRef{_current_module, token.make_slice()};
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

FunctionCallExprNode &OperatorRewriter::build_operator_call(
    const std::string &spelling,
    OpFixity fixity,
    const TokenReference &at,
    std::vector<ExprNode *> operands
)
{
    auto &call = build_operator_call_node(
        *_current_module, _collector, spelling, fixity, at, std::move(operands));

    _changed = true;

    return call;
}

void OperatorRewriter::widen_binary_operands(BinaryExprNode &bin)
{
    // **the post-parse moment of the parser's binary reconciliation** - the rule itself is
    // AST::reconcile_binary_operands', shared with Parser::parse_binary_expr.
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

    // **a shift has nothing to reconcile**, and this is the site where getting that wrong was invisible:
    // BinaryExprNode::result_type() answers the left operand for one now, so the void gate below no
    // longer invites this - but the gate is the *symptom*, and the rule belongs where the rule is asked.
    // A count widened to meet the value, or the value widened to meet the count, is a shift performed at
    // a type nobody wrote
    if (bin.op_node != nullptr && !binary_reconciles_operands(bin.op_node->op)) {
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

    // **the rule is AST::reconcile_binary_operands', in one place with the parser's.** a second
    // copy here would not know which side *knows* what it is - so a literal's default would cast
    // the typed operand beside it down to meet it
    const BinaryReconciliation reconciled = reconcile_binary_operands(
        bin.op_node != nullptr ? bin.op_node->op : nullptr,
        bin.lhs,
        bin.rhs,
        _current_module->nodes);

    report_binary_reconciliation(_collector, _current_module, reconciled);

    if (reconciled.result == BinaryReconciliation::Result::t_refused) {
        // no `_changed`: nothing moved, and the identical sentence at the identical token is what
        // Collector::collect_issue de-duplicates on, so a later round reporting again is a no-op
        return;
    }

    if (reconciled.result == BinaryReconciliation::Result::t_unchanged) {
        return;
    }

    bin.lhs = reconciled.lhs;
    bin.rhs = reconciled.rhs;

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
    //. closing it is what lets a bare `[` mean exactly one thing: ask the container
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
    const auto &candidates =
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

ExprNode *OperatorRewriter::rewrite_value_edge(ExprNode *expr)
{
    if (expr == nullptr) {
        return nullptr;
    }

    // an array literal reaching here is one no *statement* claimed - visitVarDecl and visit_assign
    // hand theirs to ArrayLiteralExpansion::expand_statement and do not descend into them. so this
    // is a literal in an expression position: an argument, which a destination can type, or a
    // position nothing will ever type
    //
    // **a variadic pack is the one bracket that is not a collection**, so it is left alone by the
    // question rather than by the state: `expansion_decided` says "the rewriter is finished with this
    // one", which a pack also happens to be, and resting the C variadic path on that coincidence puts
    // it one re-reading of a three-state flag away from being expanded into an `array<E>` and appends
    if (variadic_pack_of(expr) != nullptr) {
        return expr;
    }

    if (auto *literal = array_literal_of(expr)) {
        ArrayLiteralExpansion expansion(
            *_current_module, _collector, _current_file, _finalizing, _hoist_count);

        if (ExprNode *place = expansion.expand_expression(*literal, _hoisted, _hoist_barrier)) {
            _changed = true;
            return place;
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
    // re-parse, so the substituted body still holds it.
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
    ArrayLiteralExpansion expansion(
        *_current_module, _collector, _current_file, _finalizing, _hoist_count);

    for (size_t i = 0; i < node.children.size(); i++) {
        // **ahead of both the literal expansion and the descent**, and see resolve_index_write for why
        // each of those orderings is the content rather than a preference: after the descent the bracket
        // has already been decided a read, and after the expansion a literal right-hand side is reported
        // against a destination this rewrite was about to remove
        resolve_index_write(node, i);

        // statement-level hoists are placed *before* the descent, so `_hoisted` is empty going in and
        // a nested block cannot steal them. sibling insertion shifts this statement down; the loop
        // then walks the hoists and the original in later iterations, so there is no index patch
        if (expansion.expand_statement(node, i, _hoisted)) {
            _changed = true;
        }

        if (!_hoisted.empty()) {
            place_array_literal_hoists(node, i, *_current_module, _hoisted);
        }

        // saved around the descent, because a nested block runs this very loop: a literal hoisted
        // inside it belongs to *its* statement
        std::vector<NodeReference> outer;
        outer.swap(_hoisted);

        statement_edge(node.children[i].node());

        if (!_hoisted.empty()) {
            place_array_literal_hoists(node, i, *_current_module, _hoisted);
        }

        _hoisted.swap(outer);
    }
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
    statement_edge(node.target_bind);
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

    // **the stored result type, refreshed every round.** it cannot be derived at the ask - wrapping a
    // payload with no null value of its own interns a layout and `result_type()` has no registry - so this
    // is where a continuation that has since become concrete reaches the node that answers for it. a
    // generic's `$box?->get()` is a bare `T` until the instance exists, and the round after that it is not
    const ValueType refreshed = optional_chain_result_type(node.continuation, _collector.type_registry);

    if (!(refreshed == node.result)) {
        node.result = refreshed;
        _changed = true;
    }

    ExprNode *base = upgrade_optional_operand(node.base, node.token);

    if (base == node.base) {
        return;
    }

    node.base = base;

    // **and the marker's stored type, which nothing else can repair.** a ChainBaseNode holds the
    // base's non-null type rather than an edge to the base, and clone only *substitutes* it - so a
    // marker minted for a bare `T` becomes `weak<Node>` where it should have become `Node`. re-derived
    // here, at the one moment the base is known to have changed shape (it cannot be
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
