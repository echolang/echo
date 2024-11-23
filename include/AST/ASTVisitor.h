#ifndef ASTVISITOR_H
#define ASTVISITOR_H

#pragma once

namespace AST 
{
    class ScopeNode;
    class TypeNode;
    class TypeCastNode;
    class VarDeclNode;
    class VarRefNode;
    class LiteralFloatExprNode;
    class LiteralIntExprNode;
    class LiteralBoolExprNode;
    class LiteralStringExprNode;
    class FunctionCallExprNode;
    class AddrOfExprNode;
    class DerefExprNode;
    class PointerValueNode;
    class MoveExprNode;
    class ClassAllocExprNode;
    class RetainExprNode;
    class InstanceOfExprNode;
    class ReleaseNode;
    class IndexExprNode;
    class BinaryExprNode;
    class UnaryExprNode;
    class NullNode;
    class OperatorNode;
    class FunctionDeclNode;
    class ReturnNode;
    class IfStatementNode;
    class WhileStatementNode;
    class AssignNode;
    class NamespaceDeclNode;
    class NamespaceNode;
    class AttributeNode;
    class TypeDeclNode;
    class MemberAccessNode;
    class VarNode;

    class Visitor
    {
    public:
        virtual ~Visitor();

        virtual void visitScope(ScopeNode &node) = 0;
        virtual void visitType(TypeNode &node) = 0;
        virtual void visitTypeCast(TypeCastNode &node) = 0;
        virtual void visitVarDecl(VarDeclNode &node) = 0;
        virtual void visitVarRef(VarRefNode &node) = 0;
        virtual void visitLiteralFloatExpr(LiteralFloatExprNode &node) = 0;
        virtual void visitLiteralIntExpr(LiteralIntExprNode &node) = 0;
        virtual void visitLiteralBoolExpr(LiteralBoolExprNode &node) = 0;
        virtual void visitLiteralStringExpr(LiteralStringExprNode &node) = 0;
        virtual void visitFunctionCallExpr(FunctionCallExprNode &node) = 0;
        virtual void visit_addr_of_expr(AddrOfExprNode &node) = 0;
        virtual void visit_deref_expr(DerefExprNode &node) = 0;
        virtual void visit_pointer_value(PointerValueNode &node) = 0;
        virtual void visit_move_expr(MoveExprNode &node) = 0;
        virtual void visit_class_alloc_expr(ClassAllocExprNode &node) = 0;
        virtual void visit_retain_expr(RetainExprNode &node) = 0;
        virtual void visit_instanceof_expr(InstanceOfExprNode &node) = 0;
        virtual void visit_release(ReleaseNode &node) = 0;
        virtual void visit_index_expr(IndexExprNode &node) = 0;
        virtual void visitBinaryExpr(BinaryExprNode &node) = 0;
        virtual void visitUnaryExpr(UnaryExprNode &node) = 0;
        virtual void visitNull(NullNode &node) = 0;
        virtual void visitOperator(OperatorNode &node) = 0;
        virtual void visitFunctionDecl(FunctionDeclNode &node) = 0;
        virtual void visitReturn(ReturnNode &node) = 0;
        virtual void visitIfStatement(IfStatementNode &node) = 0;
        virtual void visitWhileStatement(WhileStatementNode &node) = 0;
        virtual void visit_assign(AssignNode &node) = 0;
        virtual void visitNamespaceDecl(NamespaceDeclNode &node) = 0;
        virtual void visitNamespace(NamespaceNode &node) = 0;
        virtual void visitAttribute(AttributeNode &node) = 0;
        virtual void visit_type_decl(TypeDeclNode &node) = 0;
        virtual void visitMemberAccess(MemberAccessNode &node) = 0;
        virtual void visitVar(VarNode &node) = 0;
    };
};

#endif