#ifndef ASTRECURSIVEVISITOR_H
#define ASTRECURSIVEVISITOR_H

#pragma once

#include "ASTVisitor.h"

namespace AST
{
    // a read-only walker over the AST. every visit method descends into the node's owned children
    // (mirroring the child edges enumerated authoritatively in src/AST/ASTClone.cpp) and does
    // nothing else. subclass this for a pass that only cares about a few node kinds: override the
    // interesting visit* methods, do the work, then call the RecursiveVisitor:: base to continue the
    // descent. cross-reference edges (a VarNode's decl, a call's decl) are deliberately not followed
    // so the walk stays a tree traversal and terminates.
    class RecursiveVisitor : public Visitor
    {
    public:
        virtual ~RecursiveVisitor();

        void visitScope(ScopeNode &node) override;
        void visitType(TypeNode &node) override;
        void visitTypeCast(TypeCastNode &node) override;
        void visitVarDecl(VarDeclNode &node) override;
        void visitVarRef(VarRefNode &node) override;
        void visitLiteralFloatExpr(LiteralFloatExprNode &node) override;
        void visitLiteralIntExpr(LiteralIntExprNode &node) override;
        void visitLiteralBoolExpr(LiteralBoolExprNode &node) override;
        void visitLiteralStringExpr(LiteralStringExprNode &node) override;
        void visitFunctionCallExpr(FunctionCallExprNode &node) override;
        void visit_addr_of_expr(AddrOfExprNode &node) override;
        void visit_deref_expr(DerefExprNode &node) override;
        void visit_pointer_value(PointerValueNode &node) override;
        void visit_move_expr(MoveExprNode &node) override;
        void visit_class_alloc_expr(ClassAllocExprNode &node) override;
        void visit_retain_expr(RetainExprNode &node) override;
        void visit_instanceof_expr(InstanceOfExprNode &node) override;
        void visit_release(ReleaseNode &node) override;
        void visit_index_expr(IndexExprNode &node) override;
        void visitBinaryExpr(BinaryExprNode &node) override;
        void visitUnaryExpr(UnaryExprNode &node) override;
        void visitNull(NullNode &node) override;
        void visitOperator(OperatorNode &node) override;
        void visitFunctionDecl(FunctionDeclNode &node) override;
        void visitReturn(ReturnNode &node) override;
        void visitIfStatement(IfStatementNode &node) override;
        void visitWhileStatement(WhileStatementNode &node) override;
        void visit_assign(AssignNode &node) override;
        void visitNamespaceDecl(NamespaceDeclNode &node) override;
        void visitNamespace(NamespaceNode &node) override;
        void visitAttribute(AttributeNode &node) override;
        void visit_type_decl(TypeDeclNode &node) override;
        void visitMemberAccess(MemberAccessNode &node) override;
        void visitVar(VarNode &node) override;
    };
};

#endif
