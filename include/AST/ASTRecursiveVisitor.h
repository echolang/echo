#ifndef ASTRECURSIVEVISITOR_H
#define ASTRECURSIVEVISITOR_H

#pragma once

#include "AST/ASTVisitor.h"
#include "AST/ASTNodeReference.h"

#include <vector>

namespace AST
{
    class ExprNode;

    // a walker over the AST. every visit method descends into the node's owned children (mirroring the
    // child edges enumerated authoritatively in src/AST/ASTClone.cpp) and does nothing else. subclass
    // this for a pass that only cares about a few node kinds: override the interesting visit* methods,
    // do the work, then call the RecursiveVisitor:: base to continue the descent. cross-reference edges
    // (a VarNode's decl, a call's decl) are deliberately not followed so the walk stays a tree
    // traversal and terminates
    //
    // **this is the sole owner of two facts**, and both are about the node's shape rather than about
    // any pass's policy:
    //
    //  - *what owned edges does this node have* - already true before the seams below existed, and the
    //    reason ForeachLowering could be built on it;
    //  - *which of those positions want a place rather than a read* - the operand of `&`, of a deref,
    //    of a `:$`, an index base and a `->` base. a different question from AST::is_place_expression,
    //    which is about an expression rather than about the position it sits in
    //
    // the totality is the point. every visit* is pure virtual on AST::Visitor, so a node kind added
    // without a method does not compile - where a hand-rolled `switch` over NodeType ending in a
    // `default:` silently treats the new kind as a leaf and never visits its subtree. that failure
    // mode cost this compiler two live bugs (a `foreach` in a guard's else arm, and every pointer read
    // inside a `??`, a `?->` or a `strong`), which is why the two rewriters are built on this now
    class RecursiveVisitor : public Visitor
    {
    public:
        virtual ~RecursiveVisitor();

        void visitScope(ScopeNode &node) override;
        void visitType(TypeNode &node) override;
        void visitTypeCast(TypeCastNode &node) override;
        void visitVarDecl(VarDeclNode &node) override;
        void visit_const_decl(ConstDeclNode &node) override;
        void visitVarRef(VarRefNode &node) override;
        void visitLiteralFloatExpr(LiteralFloatExprNode &node) override;
        void visitLiteralIntExpr(LiteralIntExprNode &node) override;
        void visitLiteralBoolExpr(LiteralBoolExprNode &node) override;
        void visitLiteralStringExpr(LiteralStringExprNode &node) override;
        void visit_string_interpolation(StringInterpolationExprNode &node) override;
        void visit_static_property(StaticPropertyExprNode &node) override;
        void visitFunctionCallExpr(FunctionCallExprNode &node) override;
        void visit_addr_of_expr(AddrOfExprNode &node) override;
        void visit_deref_expr(DerefExprNode &node) override;
        void visit_pointer_value(PointerValueNode &node) override;
        void visit_move_expr(MoveExprNode &node) override;
        void visit_const_ref(ConstRefExprNode &node) override;
        void visit_class_alloc_expr(ClassAllocExprNode &node) override;
        void visit_retain_expr(RetainExprNode &node) override;
        void visit_strong_expr(StrongExprNode &node) override;
        void visit_guard(GuardNode &node) override;
        void visit_null_coalesce(NullCoalesceExprNode &node) override;
        void visit_optional_chain(OptionalChainExprNode &node) override;
        void visit_chain_base(ChainBaseNode &node) override;
        void visit_closure_expr(ClosureExprNode &node) override;
        void visit_indirect_call_expr(IndirectCallExprNode &node) override;
        void visit_instanceof_expr(InstanceOfExprNode &node) override;
        void visit_temporary_bind(TemporaryBindExprNode &node) override;
        void visit_release(ReleaseNode &node) override;
        void visit_index_expr(IndexExprNode &node) override;
        void visit_array_literal_expr(ArrayLiteralExprNode &node) override;
        void visitBinaryExpr(BinaryExprNode &node) override;
        void visitUnaryExpr(UnaryExprNode &node) override;
        void visitNull(NullNode &node) override;
        void visitOperator(OperatorNode &node) override;
        void visitFunctionDecl(FunctionDeclNode &node) override;
        void visitReturn(ReturnNode &node) override;
        void visitIfStatement(IfStatementNode &node) override;
        void visit_const_if(ConstIfNode &node) override;
        void visit_const_expr(ConstExprNode &node) override;
        void visitWhileStatement(WhileStatementNode &node) override;
        void visit_for_statement(ForStatementNode &node) override;
        void visit_loop_control(LoopControlNode &node) override;
        void visit_foreach(ForeachNode &node) override;
        void visit_assign(AssignNode &node) override;
        void visitNamespaceDecl(NamespaceDeclNode &node) override;
        void visitNamespace(NamespaceNode &node) override;
        void visitAttribute(AttributeNode &node) override;
        void visit_type_decl(TypeDeclNode &node) override;
        void visitMemberAccess(MemberAccessNode &node) override;
        void visitVar(VarNode &node) override;

    protected:
        // **the one thing a walk can differ about: what happens to the edge itself.** the default is
        // "descend, and keep what was there", which is exactly a read-only walk - so a pass that only
        // reads overrides neither and notices nothing.
        //
        // a *rewriter* overrides these two and inherits a traversal it did not have to write, and
        // therefore cannot forget an arm of. descent and transformation are one call deliberately: a
        // pass that wrapped an edge would otherwise have to remember to descend into it first, and the
        // order of those two is not free (AST::PointerAdjuster's deref goes *outside* whatever the
        // subtree became)
        virtual ExprNode *rewrite_value_edge(ExprNode *expr);
        virtual ExprNode *rewrite_place_edge(ExprNode *expr);

        // one helper per edge *shape*, because the edges are not one shape - an owned expression is a
        // bare `ExprNode *` in most nodes, a NodeReference in a scope's children and under a `->`, and
        // a vector of either. erasing them to a single mutable handle would trade away the static
        // typing for a `set()` that has to blind-cast, which is a worse bargain than the one it fixes
        void value_edge(ExprNode *&edge);
        void value_edge(NodeReference &edge);
        void value_edges(std::vector<ExprNode *> &edges);
        void place_edge(ExprNode *&edge);
        void place_edge(NodeReference &edge);

        // a statement, a scope or a declaration: descended into, never replaced. no pass replaces one
        // of *these* edges - the ones that replace statements do it through a scope's child list, which
        // is visitScope's business and stays there.
        //
        // **virtual, which makes it the third seam.** the two above are "what happens to this expression
        // edge"; this is "what happens to this statement edge", and between them every owned edge the base
        // descends passes through one or the other. that totality is what a walk needs to be able to reach
        // *every* node in a subtree - which is what AST::ConstFolding needs before it discards one, since
        // NodeCollection::forget is unsound if it is given anything less than the whole of what went away.
        // a read-only walk overrides neither and notices nothing
        virtual void statement_edge(Node *node);
        void statement_edges(NodeReferenceList &edges);

    private:
        // the NodeReference forms of the two edge helpers. a reference that does not hold an
        // expression is a statement edge instead - the gate lives here so its one owner is this file
        void rewrite_ref_edge(NodeReference &edge, bool as_place);
    };
};

#endif
