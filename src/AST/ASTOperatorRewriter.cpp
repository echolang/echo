#include "AST/ASTOperatorRewriter.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTIssue.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTNamespace.h"
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

            if (file.root != nullptr) {
                rewrite(file.root);
            }
        }
    }

    return _changed;
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

    // asked before the operand types, because it is the cheap half and it is false for all but a
    // handful of nodes: already reconciled - by the parser, or by an earlier round of this.
    //
    // a void answer is what "not reconciled" looks like from the outside; anything else is either not
    // ready yet or not this function's business, since a pointer, a class and a nullable all have their
    // own arms in BinaryExprNode::result_type() and never reach a void answer through width alone
    if (!bin.result_type().is_void()) {
        return;
    }

    const ValueType left = value_type_of(bin.lhs->result_type());
    const ValueType right = value_type_of(bin.rhs->result_type());

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

    const std::string decorated_name =
        operator_function_name(OperatorRegistry::bracket_spelling(), OpFixity::t_index);

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
    // writes them in: `operator (Array<T>& $a)[usize $i]`. the receiver is *not* addressed here - the
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

    // a declaration's initializer: `Array<int32> $a = [1, 2, 3];`
    if (statement->get_node_type() == NodeType::n_vardecl) {
        auto *decl = static_cast<VarDeclNode *>(statement);

        if (decl->init_expr != nullptr
            && decl->init_expr->get_node_type() == NodeType::n_expr_array_literal) {
            return {decl, static_cast<ArrayLiteralExprNode *>(decl->init_expr), &decl->init_expr};
        }

        return {};
    }

    // an assignment's right-hand side, and **only over a plain variable**: `$a = [1, 2, 3];`. the
    // expansion needs one fresh place per append, and a variable is what can be respelled from its
    // declaration. any other target - `$s->items = [...]` - would need the target subtree cloned per
    // element, which is A13b's temporary problem wearing different clothes
    if (statement->get_node_type() == NodeType::n_assign) {
        auto *assign = static_cast<AssignNode *>(statement);

        if (assign->value_expr == nullptr
            || assign->value_expr->get_node_type() != NodeType::n_expr_array_literal) {
            return {};
        }

        auto *literal = static_cast<ArrayLiteralExprNode *>(assign->value_expr);

        if (assign->target == nullptr || assign->target->get_node_type() != NodeType::n_varref) {
            return {nullptr, literal, nullptr};
        }

        return {place_root_of(assign->target), literal, &assign->value_expr};
    }

    return {};
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

    const ValueType destination_type =
        destination.decl->has_type() ? destination.decl->type() : ValueType::make_unknown();

    // not decided yet: the declaration may be typed from a call this fixpoint has not settled
    if (is_undetermined_type(destination_type)) {
        return;
    }

    literal.expansion_decided = true;

    // has_property_layout, not has_complex_type: the expansion below asks the destination for a
    // zero-argument constructor and appends into the places it makes, and an interface declares
    // neither. so an array literal assigned to an interface-typed destination is the "cannot be built
    // from a literal" diagnostic below rather than a lookup that finds nothing
    ComplexType *ct = destination_type.has_property_layout() ? destination_type.get_complex_type() : nullptr;

    if (ct == nullptr) {
        _collector.collect_issue<Issue::GenericError>(
            code_ref_for(literal.token_bracket),
            fmt::format(
                "'{}' cannot be built from an array literal - a literal fills a collection, through "
                "its constructor and its append operator.",
                destination_type.get_type_desciption()));
        return;
    }

    // **the constructor of the destination type, named rather than looked up.** an instantiation
    // carries its template's name and its own type arguments, which is exactly what a call site
    // writes as `Array<int32>()` - so this builds the call a user would have written and lets
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

    *destination.slot = &ctor;

    // one `$dest[] = element` per element, spliced in after the statement being expanded. each gets
    // its **own** place naming the destination - one subtree per append, because PointerAdjuster
    // rewrites edges in place and a shared one would be adjusted once per use
    std::vector<NodeReference> appends;
    appends.reserve(literal.elements.size());

    for (auto *element : literal.elements) {
        auto &var = _current_module->nodes.emplace_back<VarNode>(destination.decl);
        auto &var_ref = _current_module->nodes.emplace_back<VarRefNode>(&var);

        auto &slot = _current_module->nodes.emplace_back<IndexExprNode>(
            &var_ref, std::vector<ExprNode *>{}, literal.token_bracket);

        // the two facts the parser records for a hand-written `$a[] = v`, recorded here for the same
        // reasons: the slot is bound rather than read, and the write into it initializes storage
        // that holds nothing, so no teardown is owed
        slot.slot_is_bound = true;

        auto &assign = _current_module->nodes.emplace_back<AssignNode>(
            &slot, element, literal.token_bracket);
        assign.is_initialization = true;

        appends.push_back(make_ref(assign));
    }

    scope.children.insert(scope.children.begin() + index + 1, appends.begin(), appends.end());

    _changed = true;
}

ExprNode *OperatorRewriter::rewrite_expr(ExprNode *expr)
{
    if (expr == nullptr) {
        return nullptr;
    }

    // an array literal reaching here is one no statement claimed - the vardecl and assign arms hand
    // theirs to expand_array_literal above and do not descend into them. so this position cannot
    // hold one, whatever it is
    if (expr->get_node_type() == NodeType::n_expr_array_literal) {
        report_unplaced_literal(*static_cast<ArrayLiteralExprNode *>(expr));
        return expr;
    }

    rewrite(expr);

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

void OperatorRewriter::rewrite(Node *node)
{
    if (node == nullptr) {
        return;
    }

    switch (node->get_node_type()) {
        case NodeType::n_scope:
        {
            // indexed rather than ranged, because expand_array_literal splices siblings in - and
            // *before* the child is rewritten, so the constructor and the appends it produces are
            // walked by this same pass rather than waiting a round
            auto *scope = static_cast<ScopeNode *>(node);

            for (size_t i = 0; i < scope->children.size(); i++) {
                expand_array_literal(*scope, i);
                rewrite(scope->children[i].node());
            }
            break;
        }

        case NodeType::n_func_decl:
        {
            // a template's body is only meaningful once cloned into a concrete instance, and its
            // operand types are the very things that are not known there. PointerAdjuster's rule
            auto *fn = static_cast<FunctionDeclNode *>(node);
            if (!fn->is_generic()) {
                rewrite(fn->body);
            }
            break;
        }

        case NodeType::n_vardecl:
        {
            // an array literal initializer belongs to the enclosing scope, which has already had its
            // turn at it above - descending here would report it as an expression in a position that
            // cannot hold one
            auto *decl = static_cast<VarDeclNode *>(node);

            if (decl->init_expr != nullptr
                && decl->init_expr->get_node_type() == NodeType::n_expr_array_literal) {
                break;
            }

            decl->init_expr = rewrite_expr(decl->init_expr);
            break;
        }

        case NodeType::n_assign:
        {
            auto *assign = static_cast<AssignNode *>(node);
            assign->target = rewrite_expr(assign->target);

            // as for a declaration above: the scope owns an array literal on the right-hand side
            if (assign->value_expr == nullptr
                || assign->value_expr->get_node_type() != NodeType::n_expr_array_literal) {
                assign->value_expr = rewrite_expr(assign->value_expr);
            }

            rewrite(assign->teardown_old);
            break;
        }

        case NodeType::n_expr_binary:
        {
            auto *bin = static_cast<BinaryExprNode *>(node);
            bin->lhs = rewrite_expr(bin->lhs);
            bin->rhs = rewrite_expr(bin->rhs);
            widen_binary_operands(*bin);
            break;
        }

        case NodeType::n_expr_unary:
        {
            auto *un = static_cast<UnaryExprNode *>(node);
            un->expr = rewrite_expr(un->expr);
            break;
        }

        case NodeType::n_expr_call:
        {
            auto *call = static_cast<FunctionCallExprNode *>(node);
            for (auto *&arg : call->arguments) {
                arg = rewrite_expr(arg);
            }
            break;
        }

        case NodeType::n_expr_indirect_call:
        {
            auto *call = static_cast<IndirectCallExprNode *>(node);
            call->callee = rewrite_expr(call->callee);
            for (auto *&arg : call->arguments) {
                arg = rewrite_expr(arg);
            }
            break;
        }

        case NodeType::n_expr_closure:
        {
            auto *closure = static_cast<ClosureExprNode *>(node);
            for (auto *&captured : closure->captured_values) {
                captured = rewrite_expr(captured);
            }
            break;
        }

        case NodeType::n_expr_addrof:
        {
            auto *addr = static_cast<AddrOfExprNode *>(node);
            addr->operand = rewrite_expr(addr->operand);
            break;
        }

        case NodeType::n_expr_deref:
        {
            auto *deref = static_cast<DerefExprNode *>(node);
            deref->operand = rewrite_expr(deref->operand);
            break;
        }

        case NodeType::n_expr_index:
        {
            auto *index_expr = static_cast<IndexExprNode *>(node);

            // the operands first: `$a[$b[0]]` has to decide the inner bracket before the outer one
            // can read a type off it
            index_expr->base = rewrite_expr(index_expr->base);
            for (auto *&index : index_expr->indices) {
                index = rewrite_expr(index);
            }

            rewrite(index_expr->element_call);

            resolve_index(*index_expr);
            break;
        }

        case NodeType::n_expr_peel:
        {
            auto *peel = static_cast<PointerValueNode *>(node);
            peel->operand = rewrite_expr(peel->operand);
            break;
        }

        case NodeType::n_member_access:
        {
            // **reseated**, unlike PointerAdjuster's arm: rule 3 replaces a node rather than editing
            // it, so a base that is a binary expression over a declared operator becomes a different
            // node entirely and the reference has to follow it
            auto *access = static_cast<MemberAccessNode *>(node);
            NodeReference &base = access->get_base_node();

            if (base.has() && base.is_expression_node()) {
                auto *rewritten = rewrite_expr(base.unsafe_ptr<ExprNode>());
                base = NodeReference(rewritten->get_node_type(), rewritten);
            }

            break;
        }

        case NodeType::n_expr_instanceof:
        {
            auto *instance_of = static_cast<InstanceOfExprNode *>(node);
            instance_of->operand = rewrite_expr(instance_of->operand);
            break;
        }

        case NodeType::n_expr_temp_bind:
        {
            // an operator or a bracket can sit anywhere inside a bound subtree, so all three edges get
            // their round - a temporary is bound around a member chain, and `$o->mid()->tag + 1` puts a
            // declared `+` directly above one
            auto *bind = static_cast<TemporaryBindExprNode *>(node);

            for (VarDeclNode *temp : bind->temporaries) {
                rewrite(temp);
            }

            bind->body = rewrite_expr(bind->body);

            for (auto &drop : bind->teardown) {
                rewrite(drop.node());
            }
            break;
        }

        case NodeType::n_expr_retain:
        {
            auto *retain = static_cast<RetainExprNode *>(node);
            retain->operand = rewrite_expr(retain->operand);
            break;
        }

        case NodeType::n_type_cast:
        {
            auto *cast = static_cast<TypeCastNode *>(node);
            cast->expr = rewrite_expr(cast->expr);
            break;
        }

        case NodeType::n_func_return:
        {
            auto *ret = static_cast<ReturnNode *>(node);
            ret->expr = rewrite_expr(ret->expr);

            // the drops a return owes, which ride on the node rather than sitting ahead of it
            for (auto &drop : ret->unwind) {
                rewrite(drop.node());
            }
            break;
        }

        case NodeType::n_foreach:
        {
            // **mandatory, and the failure is a deadlock rather than a wrong answer.** this switch ends
            // in a `default:` that treats an unknown tag as a leaf, so without this arm
            // `foreach ($grid[0] as $row)`'s bracket never becomes an `operator []` call, the source
            // type never resolves, AST::ForeachLowering waits forever and the program compiles to
            // nothing with no diagnostic at all
            auto *loop = static_cast<ForeachNode *>(node);
            loop->source = rewrite_expr(loop->source);
            rewrite(loop->body);
            break;
        }

        case NodeType::n_loop_control:
        {
            // the same list on the other early exit. **mandatory**: this switch ends in a `default:`
            // that treats an unknown tag as a leaf, so leaving it out is silent - an operator or a
            // bracket inside a drop would never be rewritten, and a BinaryExprNode surviving to
            // gen_binary_expr emits a scaled GEP rather than the call the author declared
            //
            // AST::PointerAdjuster deliberately has no matching arm, and neither does it walk a return's
            // unwind: AST::needs_destruction says a pointer is not an owner, so a drop's place never
            // reaches through one and there is no deref to insert. do not add one "for symmetry"
            auto *exit_node = static_cast<LoopControlNode *>(node);

            for (auto &drop : exit_node->unwind) {
                rewrite(drop.node());
            }
            break;
        }

        case NodeType::n_if_statement:
        {
            auto *stmt = static_cast<IfStatementNode *>(node);
            stmt->condition = rewrite_expr(stmt->condition);
            rewrite(stmt->if_scope);
            rewrite(stmt->else_scope);
            break;
        }

        case NodeType::n_while_statement:
        {
            auto *stmt = static_cast<WhileStatementNode *>(node);
            stmt->condition = rewrite_expr(stmt->condition);
            rewrite(stmt->loop_scope);
            break;
        }

        default:
            // leaves: literals, operators, var references, types, releases. nothing to rewrite
            break;
    }
}

};
