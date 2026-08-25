#include "AST/ASTConstFolding.h"

#include "AST/ASTBundle.h"
#include "AST/ASTCollector.h"
#include "AST/ASTConstFold.h"
#include "AST/ASTDetach.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
#include "AST/ASTModule.h"
#include "AST/ConstExprNode.h"
#include "AST/ConstIfNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/ScopeNode.h"

#include <fmt/core.h>

#include <unordered_set>

namespace AST
{

ConstFolding::ConstFolding(Bundle &bundle)
    : _bundle(bundle), _collector(bundle.collector)
{
}

CodeRef ConstFolding::code_ref_for(const TokenReference &token)
{
    return CodeRef{_current_module, token.make_slice()};
}

bool ConstFolding::run_round()
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

    // **once for the round, not once per discard.** Bundle::forget_nodes sweeps every bucket of every
    // module, and a round folds one subtree away per `const if` and per `const(...)` - which with the
    // stdlib is several per generic instantiation. nothing between a discard and here reads
    // NodeCollection::of_type: this walk goes through scope children, and the sweep that still cares -
    // TypeLowering::build_function_maps - is codegen. Monomorphizer::snapshot_calls walks the live tree
    _detached.flush(_bundle);

    return _changed;
}

void ConstFolding::forget(Node &root)
{
    _detached.collect(root);
}

void ConstFolding::finalize()
{
    // one more round, rather than a sweep: a round inherits visitFunctionDecl's generic-body skip - so a
    // `const if` inside a template nobody instantiated is not blamed for a `T` that was never going to be
    // substituted - and it walks scope children, where a sweep would see detached nodes forever
    _finalizing = true;
    run_round();
    _finalizing = false;
}

void ConstFolding::visitFunctionDecl(FunctionDeclNode &node)
{
    if (!node.is_generic()) {
        RecursiveVisitor::visitFunctionDecl(node);
    }
}

void ConstFolding::visitScope(ScopeNode &node)
{
    for (size_t i = 0; i < node.children.size(); i++) {
        if (node.children[i].has_type<ConstIfNode>()) {
            lower(node, i);
        }

        // re-read rather than cached: lower() reseats the edge, and the arm it seated is what has to be
        // descended into - which is what lets a nested `const if`, and an `else const if` chain, settle in
        // this same round
        if (node.children[i].has()) {
            node.children[i].node()->accept(*this);
        }
    }
}

ExprNode *ConstFolding::rewrite_value_edge(ExprNode *expr)
{
    // the descent first, so a nested `const(...)` is already a literal by the time this one is asked -
    // and so the operand of the one being folded has had its own edges rewritten
    expr = RecursiveVisitor::rewrite_value_edge(expr);

    if (expr == nullptr || expr->get_node_type() != NodeType::n_expr_const) {
        return expr;
    }

    auto &marker = static_cast<ConstExprNode &>(*expr);
    const ConstFoldResult folded = const_fold(marker.operand);

    if (folded.result == ConstFoldResult::Result::t_pending) {
        // **out of rounds is out of answers** - see finalize(). the message is only for the case where
        // nothing else explained it, which has_critical_issues() is already the compiler's answer to
        if (_finalizing) {
            if (_collector.has_critical_issues()) {
                return refuse_expr(marker, std::nullopt);
            }

            return refuse_expr(marker,
                "this 'const' expression never became answerable - nothing ever gave its operand a "
                "type the compiler could work with.");
        }

        return expr;                                    // ask again next round
    }

    if (folded.result == ConstFoldResult::Result::t_refused) {
        return refuse_expr(marker, folded.refusal);
    }

    ExprNode *value = literal_for(folded, marker.token_const);

    if (value == nullptr) {
        return refuse_expr(marker, fmt::format(
            "a '{}' has no literal spelling, so there is nothing for this 'const' to become.",
            folded.type.get_type_desciption()));
    }

    detach(marker);

    return value;
}

ExprNode *ConstFolding::refuse_expr(ConstExprNode &marker, std::optional<std::string> why)
{
    if (why.has_value()) {
        _collector.collect_issue<Issue::GenericError>(code_ref_for(marker.token_const), std::move(*why));
    }

    detach(marker);

    // decided either way, because a survivor is what PointerAdjuster throws for. a void is what
    // AST::ConstantExpander leaves behind for a refused constant, and for the same reason
    return &_current_module->nodes.emplace_back<VoidExprNode>();
}

void ConstFolding::detach(ConstExprNode &marker)
{
    // the marker and everything under it are replaced by whatever this returns to, so the arena stops
    // answering for them - the two ownership builtins a `const(...)` folded are calls nothing should
    // instantiate now. one function, because a refusal owes this exactly as much as a fold does: a
    // refused operand still names calls TypeLowering would emit if they stayed in the arena
    forget(marker);

    // and nulled for AST::ForeachLowering's reason: PointerAdjuster rewrites edges in place, so a subtree
    // reachable from two parents would collect a deref per use
    marker.operand = nullptr;

    _changed = true;
}

ExprNode *ConstFolding::literal_for(const ConstFoldResult &folded, const TokenReference &at)
{
    if (folded.type.is_boolean_type()) {
        return &_current_module->nodes.emplace_back<LiteralBoolExprNode>(_current_module->make_virtual_token(
            folded.as_bool() ? "true" : "false", Token::Type::t_bool_literal, at));
    }

    if (!folded.type.is_integer_type()) {
        return nullptr;
    }

    // **the decimal text, not the bits**: LiteralIntExprNode reads its value back out of the token, so the
    // rendering has to be the one the accessors will parse. signed values are rendered signed, which is
    // exactly what ConstFoldResult's sign-extension invariant makes possible
    const std::string text = folded.type.is_signed_integer()
        ? std::to_string(folded.as_signed())
        : std::to_string(folded.bits);

    return &_current_module->nodes.emplace_back<LiteralIntExprNode>(
        _current_module->make_virtual_token(text, Token::Type::t_integer_literal, at),
        folded.type.get_primitive_type());
}

void ConstFolding::lower(ScopeNode &scope, size_t index)
{
    auto *branch = scope.children[index].get_ptr<ConstIfNode>();

    // if_scope may be null: clone of a generic body drops the dead arm, so a false condition
    // leaves only else_scope. ConstFolding is still the splicer
    if (branch == nullptr || branch->condition == nullptr) {
        return;
    }

    const ConstFoldResult folded = const_fold(branch->condition);

    if (folded.result == ConstFoldResult::Result::t_pending) {
        // **out of rounds is out of answers** - see finalize(). the discard is the half that matters: a
        // survivor is the InternalCompilerException PointerAdjuster throws, ahead of the gate that would
        // have printed whatever *did* explain the condition. the message is only for the case where
        // nothing else did, which has_critical_issues() is already the compiler's answer to
        if (_finalizing) {
            if (_collector.has_critical_issues()) {
                discard(scope, index, *branch);
            }
            else {
                refuse(scope, index, *branch,
                    "this 'const if' never became answerable - nothing ever gave its condition a type the "
                    "compiler could decide from. only something the compiler works out for itself can "
                    "select an arm.");
            }
        }

        return;                                         // ask again next round
    }

    if (folded.result == ConstFoldResult::Result::t_refused) {
        refuse(scope, index, *branch, folded.refusal);
        return;
    }

    // **the bool requirement belongs here, not in the folder.** const_fold answers what an expression
    // *is*; only this statement needs one particular type of it
    if (!folded.is_bool()) {
        refuse(scope, index, *branch, fmt::format(
            "a 'const if' condition has to be a 'bool', and this one works out to a '{}'. compare it "
            "against something rather than branching on the value itself.",
            folded.type.get_type_desciption()));
        return;
    }

    ScopeNode *taken = taken_const_if_arm(*branch, folded.as_bool());

    if (taken == nullptr) {
        // a false condition with no `else`. an **empty scope** rather than erasing the child, for
        // discard()'s reason: this walk holds an index and so does every other reader of a child list, so
        // removing an element would reseat every sibling behind it. codegen emits nothing at all for an
        // empty ScopeNode, so the whole cost is one node in the arena
        taken = &_current_module->nodes.emplace_back<ScopeNode>();
    }

    splice(scope, index, *branch, *taken);
}

void ConstFolding::splice(ScopeNode &scope, size_t index, ConstIfNode &branch, ScopeNode &arm)
{
    // **the lexical namespaces need nothing, and that is not luck.** a `const if` opens no lexical scope of
    // its own - only its arms' braces do, through Parser::parse_scope's AST::LexicalScope - so the arm's
    // namespace is keyed on the arm's own brace in the *enclosing* block's lexical children. it was already
    // a direct lexical child of the scope this moves it into, so its new place in the tree is exactly the
    // place it already had in the namespace hierarchy.
    //
    // the same fact is what makes the **untaken** arm safe to leave registered: anything it declared lives
    // in a namespace keyed on a brace no surviving call's lookup_namespace walks outward through, so
    // nothing can name it, and no scope holds the declaration any more so nothing emits it either.
    //
    // parent_ptr is set anyway rather than relied upon: a parsed tree and a cloned one are two producers,
    // and the empty scope lower() may have minted has no parent at all
    arm.parent_ptr = &scope;

    // **the arm being kept is released first, and the order is the whole of what makes the sweep below
    // sound.** NodeCollection::forget is told everything reachable from the branch, so anything still
    // hanging off it would be forgotten too - and forgetting a node the tree still holds is a symbol
    // nothing declares. releasing the taken arm before collecting is what keeps the walk to exactly what
    // is going away. AST::ForeachLowering states the same rule for its own reason ("one subtree per use
    // holds by construction"), and PointerAdjuster's in-place edge rewriting is the other half of it
    if (branch.if_scope == &arm) {
        branch.if_scope = nullptr;
    }

    if (branch.else_scope == &arm) {
        branch.else_scope = nullptr;
    }

    forget(branch);

    scope.children[index] = make_ref(arm);

    branch.condition = nullptr;
    branch.if_scope = nullptr;
    branch.else_scope = nullptr;

    _changed = true;
}



void ConstFolding::refuse(ScopeNode &scope, size_t index, ConstIfNode &branch, std::string why)
{
    _collector.collect_issue<Issue::GenericError>(code_ref_for(branch.token_const), std::move(why));

    discard(scope, index, branch);
}

void ConstFolding::discard(ScopeNode &scope, size_t index, ConstIfNode &branch)
{
    // **neither arm is kept.** which one was meant is exactly what could not be decided, so keeping either
    // would be compiling half a program on a guess - and whatever that arm had to say would be reported
    // underneath the one diagnostic that explains all of it
    //
    // splice with a scope the branch never held, which is the same thing lower() does for a false condition
    // with no `else` - so the release-before-collect order that makes the sweep sound lives in one place
    splice(scope, index, branch, _current_module->nodes.emplace_back<ScopeNode>());
}

};
