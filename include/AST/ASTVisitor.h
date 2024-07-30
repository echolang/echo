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
    class FunctionCallExprNode;
    class VarRefExprNode;
    class VarPtrExprNode;
    class BinaryExprNode;
    class UnaryExprNode;
    class NullNode;
    class OperatorNode;
    class FunctionDeclNode;
    class ReturnNode;
    class IfStatementNode;
    class WhileStatementNode;
    class VarMutNode;
    class NamespaceDeclNode;
    class NamespaceNode;

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
        virtual void visitFunctionCallExpr(FunctionCallExprNode &node) = 0;
        virtual void visitVarRefExpr(VarRefExprNode &node) = 0;
        virtual void visitVarPtrExpr(VarPtrExprNode &node) = 0;
        virtual void visitBinaryExpr(BinaryExprNode &node) = 0;
        virtual void visitUnaryExpr(UnaryExprNode &node) = 0;
        virtual void visitNull(NullNode &node) = 0;
        virtual void visitOperator(OperatorNode &node) = 0;
        virtual void visitFunctionDecl(FunctionDeclNode &node) = 0;
        virtual void visitReturn(ReturnNode &node) = 0;
        virtual void visitIfStatement(IfStatementNode &node) = 0;
        virtual void visitWhileStatement(WhileStatementNode &node) = 0;
        virtual void visitVarMut(VarMutNode &node) = 0;
        virtual void visitNamespaceDecl(NamespaceDeclNode &node) = 0;
        virtual void visitNamespace(NamespaceNode &node) = 0;
    };
}

#endif