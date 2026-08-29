#include "AST/ASTRegion.h"

#include "AST/ASTBundle.h"
#include "AST/ASTConstFold.h"
#include "AST/ASTFile.h"
#include "AST/ASTModule.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/ASTValueType.h"
#include "AST/AssignNode.h"
#include "AST/ConstExprNode.h"
#include "AST/ConstIfNode.h"
#include "AST/ConstRefExprNode.h"
#include "AST/ExprNode.h"
#include "AST/ForeachNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/GuardNode.h"
#include "AST/MatchExprNode.h"
#include "AST/ScopeNode.h"
#include "AST/StringInterpolationNode.h"
#include "AST/TypeCastNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/VarDeclNode.h"

namespace AST
{

void assert_region_accepts_mutation(FunctionDeclNode *fn, File *file)
{
    if (fn != nullptr) {
        assert_region_accepts_mutation(fn->region_state);
    } else if (file != nullptr) {
        assert_region_accepts_mutation(file->region_state);
    }
}

namespace
{
    // **the deny-list OwnershipPass walked as BodyAnswerable.** totality is the point: a new
    // transient node is one arm, and an early yes cannot be revisited. the walk stops at the
    // first unfinished node
    class BodyPending : public RecursiveVisitor
    {
    public:
        bool pending = false;

        // **the flag is monotone, so the walk stops the moment it is true.** this runs once per
        // un-answered body *and* per file root on every fixpoint round, and a body that waits k rounds
        // would otherwise pay k complete traversals to re-derive the same no.
        //
        // both descent seams are pruned - the statement loop here and the expression edge below - so
        // neither a later statement nor a sibling subtree is walked once the answer is settled. nothing
        // is rewritten, so unlike the base's loop this one may hold its bound
        void visitScope(ScopeNode &node) override
        {
            for (size_t i = 0; !pending && i < node.children.size(); i++) {
                statement_edge(node.children[i].node());
            }
        }

        ExprNode *rewrite_value_edge(ExprNode *expr) override
        {
            return pending ? expr : RecursiveVisitor::rewrite_value_edge(expr);
        }

        // a nested declaration is resolved as its own body, from the file root's children. whether
        // *it* is ready says nothing about whether this one is
        void visitFunctionDecl(FunctionDeclNode &) override
        {
        }

        // **and neither does a type declaration**, which is not flow at all. its properties are
        // ordinary VarDeclNodes, so descending into `struct Box<T> { T $value; }` finds one typed `T`
        // and answers pending for every body the struct is declared beside - a file root included
        void visit_type_decl(TypeDeclNode &) override
        {
        }

        // a constant reference stands for an expression nobody has substituted in yet, so nothing about
        // what a body owns can be answered while one is present.
        //
        // belt-and-braces, unlike the two arms below: AST::ConstantExpander runs *before* the fixpoint
        // rather than inside it, so a reference should never reach this walk at all. the arm is here so
        // the invariant enforces itself if that order ever moves - this walk answers a body exactly once,
        // ever, and an early yes cannot be revisited
        void visit_const_ref(ConstRefExprNode &) override
        {
            pending = true;
        }

        // an untyped declaration is one the monomorphizer has not re-derived yet, and a typed one
        // still mentioning a parameter is waiting on a substitution. either way there is no answer to
        // "does this own something" yet
        void visitVarDecl(VarDeclNode &node) override
        {
            if (!node.has_type() || contains_type_param(node.type())) {
                pending = true;
                return;
            }

            // an inferred local typed `void` from a call that has since settled to a real type:
            // retype copies the call's result onto the declaration next round. walking now would
            // decide "owns nothing" permanently. a memberwise constructor whose type declared
            // `init` stays pending at parse (arity may still shrink), so `$a = Foo(...)` is
            // still void on the first monomorphizer round
            if (node.type().is_void() && node.init_expr != nullptr
                && !(node.init_expr->result_type() == node.type())) {
                pending = true;
                return;
            }

            RecursiveVisitor::visitVarDecl(node);
        }

        // **a match whose patterns are not decided yet is never answerable.** the bindings are untyped
        // and have no initializer until AST::MatchResolution has said which case each arm names, so a
        // walk now would resolve the arrival of a value that is about to be replaced by a payload read
        // - permanently, this pass walking a body exactly once ever, and with nothing reporting it.
        //
        // the untyped-declaration arm above catches a *bound* arm today, the bindings being scope
        // children with no type node. this one is what covers an arm that binds nothing, where there is
        // no declaration to be untyped and the arm's value would otherwise be walked against a match
        // whose own result type is still unknown
        void visit_match(MatchExprNode &node) override
        {
            if (!node.patterns_decided) {
                pending = true;
                return;
            }

            RecursiveVisitor::visit_match(node);
        }

        // **an unexpanded array literal is never answerable**, which the declaration's type alone does
        // not say: one typed from its elements holds an *unknown* until AST::OperatorRewriter expands
        // it, and unknown is not contains_type_param. asked of the literal itself rather than of the
        // declaration holding one, so a literal in any other position counts too
        void visit_array_literal_expr(ArrayLiteralExprNode &node) override
        {
            if (!node.expansion_decided) {
                pending = true;
                return;
            }

            RecursiveVisitor::visit_array_literal_expr(node);
        }

        // **an assignment through an undecided bracket may not be a write to a place at all.**
        // AST::OperatorRewriter::resolve_index_write replaces the whole statement with one call when the
        // container declares an element-write contract, and this pass walks a body exactly once - so a walk
        // now would decide the ownership of an assignment about to leave the tree, and push a teardown onto
        // a node nothing will emit.
        //
        // the arm below already covers it through the target, since an undecided bracket is undecided
        // wherever it sits. this one is here so the invariant is *stated where it is relied on* rather than
        // inherited: it costs nothing, and it is what keeps the rewrite sound if a future node kind ever
        // parents an AssignNode somewhere a scope's child list does not reach
        void visit_assign(AssignNode &node) override
        {
            if (node.target != nullptr
                && node.target->get_node_type() == NodeType::n_expr_index
                && !static_cast<IndexExprNode *>(node.target)->resolution_decided) {
                pending = true;
                return;
            }

            RecursiveVisitor::visit_assign(node);
        }

        // an unresolved bracket has no element call yet, so there is nothing here for the arm below to
        // find - and resolving it is what *creates* the borrow of the container
        void visit_index_expr(IndexExprNode &node) override
        {
            if (!node.resolution_decided) {
                pending = true;
                return;
            }

            RecursiveVisitor::visit_index_expr(node);
        }

        // **a call that has not settled has not been fitted to its parameters**, and fitting is what
        // writes the `&` around a borrow argument. so a body walked while one is outstanding gives that
        // argument's temporary no slot, and AST::TypeChecker's guard rail reports it as a compiler bug -
        // which is exactly what it did. the round order already claims this invariant in the
        // monomorphizer ("last of the four, so every call in a body it walks has already been fitted");
        // this is where the claim is checked
        //
        // a *failed* call is terminal and counts as answerable: it has its own diagnostic, and waiting
        // for a program that cannot compile only costs it its drops
        void visitFunctionCallExpr(FunctionCallExprNode &node) override
        {
            if (!call_is_terminal(node.settlement)) {
                pending = true;
                return;
            }

            RecursiveVisitor::visitFunctionCallExpr(node);
        }

        // **an unlowered `const if` is never answerable**, and this is the arm whose absence is silent.
        // which of its two arms exists at all is not decided yet, and this pass walks a body exactly
        // once - so a walk now would resolve the ownership of statements about to be thrown away, and
        // give a `T $doomed` in the untaken arm a drop, which is one more generic call site.
        //
        // silent because walk_statement's `default:` reads `child.is_expression_node() ? ... : nullptr`,
        // and a ConstIfNode is not an expression - so without this the whole subtree would simply never
        // be walked, with no diagnostic anywhere. AST::ConstFolding runs earlier in the same round; once
        // it has, this node is gone and the arm it left behind is an ordinary scope
        void visit_const_if(ConstIfNode &) override
        {
            pending = true;
        }

        // the same for its expression sibling: an unfolded `const(...)` is a value nothing knows yet, and
        // a copy or a drop decided around it would be decided against the operand's type rather than the
        // literal's - which is the same type, but only because the node is transparent *on purpose*
        void visit_const_expr(ConstExprNode &) override
        {
            pending = true;
        }

        // **an unlowered foreach is never answerable.** it declares `$el` and `$k` with no type, and
        // the iterator declaration it will mint does not exist yet. AST::ForeachLowering runs earlier
        // in the same round; once it has, this node is gone and the scope it left behind is ordinary
        void visit_foreach(ForeachNode &) override
        {
            pending = true;
        }

        // **an undecided guard is never answerable**, and this arm's absence is silent in the one way
        // that matters: the binding is *typed* and its initializer resolves, so nothing here looks
        // unfinished. what is unfinished is how the binding gets filled - a question about the subject's
        // conformance that only AST::GuardLowering can answer, in the fixpoint - so a walk now would
        // resolve the arrival of a value about to be replaced by an `unwrap()` read. permanently: this
        // pass walks a body exactly once, ever.
        //
        // a `T?` guard is decided by the parser and never reaches this, so no existing program's
        // ownership answer moves by taking this arm
        void visit_guard(GuardNode &node) override
        {
            if (!node.plan_decided) {
                pending = true;
                return;
            }

            RecursiveVisitor::visit_guard(node);
        }

        // an undecided `&name` has no type yet. a walk now would decide the ownership of a
        // value about to be typed by a destination, and this pass walks a body exactly once
        void visit_function_ref_expr(FunctionRefExprNode &node) override
        {
            if (!node.resolved) {
                pending = true;
                return;
            }

            RecursiveVisitor::visit_function_ref_expr(node);
        }

        // **an unlowered interpolation is never answerable**, and this is the second arm whose absence
        // is silent. it stands for a chain of calls none of which exist yet - each one allocating a
        // `string` that owes a drop - so a walk now would decide the ownership of a statement whose
        // owning values have not been minted. AST::InterpolationLowering runs earlier in the same
        // round; once it has, what is left is ordinary calls
        void visit_string_interpolation(StringInterpolationExprNode &) override
        {
            pending = true;
        }

        // **an undecided written cast is never answerable.** a declared conversion is rewritten to a
        // call that may return an owner, and this pass walks a body exactly once - so a walk now
        // would decide the arrival of a coerce that is about to become a call. implicit casts never
        // ask; a built-in kind that CastResolution has already classified is plan_decided
        void visitTypeCast(TypeCastNode &node) override
        {
            if (!node.is_implcit && !node.plan_decided) {
                pending = true;
                return;
            }

            RecursiveVisitor::visitTypeCast(node);
        }
    };

    // the worklist. skip generic bodies; collect every call; a const if is condition plus taken arm
    class LiveCalls : public RecursiveVisitor
    {
    public:
        Module *module = nullptr;
        std::vector<std::pair<FunctionCallExprNode *, Module *>> calls;

        void visitFunctionDecl(FunctionDeclNode &node) override
        {
            if (node.is_generic()) {
                return;
            }

            RecursiveVisitor::visitFunctionDecl(node);
        }

        void visit_type_decl(TypeDeclNode &) override
        {
        }

        void visitFunctionCallExpr(FunctionCallExprNode &node) override
        {
            calls.push_back({&node, module});
            RecursiveVisitor::visitFunctionCallExpr(node);
        }

        void visit_const_if(ConstIfNode &node) override
        {
            value_edge(node.condition);
            statement_edge(taken_const_if_arm(node));
        }
    };
}

bool body_is_pending(ScopeNode &scope)
{
    BodyPending walk;
    scope.accept(walk);
    return walk.pending;
}

std::vector<std::pair<FunctionCallExprNode *, Module *>> live_calls(Bundle &bundle)
{
    LiveCalls walk;

    for (auto &module_ptr : bundle.modules) {
        walk.module = module_ptr.get();

        for (auto &file : module_ptr->files()) {
            if (file.root != nullptr) {
                file.root->accept(walk);
            }
        }
    }

    return walk.calls;
}

};
