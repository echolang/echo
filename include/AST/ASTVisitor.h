#ifndef ASTVISITOR_H
#define ASTVISITOR_H

#pragma once

namespace AST 
{
    class ScopeNode;
    class TypeNode;
    class VarDeclNode;
    class VarRefNode;
    class LiteralFloatExprNode;
    class LiteralIntExprNode;
    class LiteralBoolExprNode;
    class FunctionCallExprNode;
    class VarRefExprNode;
    class NullNode;

    class Visitor
    {
    public:
        virtual ~Visitor();

        virtual void visitScope(ScopeNode &node) = 0;
        virtual void visitType(TypeNode &node) = 0;
        virtual void visitVarDecl(VarDeclNode &node) = 0;
        virtual void visitVarRef(VarRefNode &node) = 0;
        virtual void visitLiteralFloatExpr(LiteralFloatExprNode &node) = 0;
        virtual void visitLiteralIntExpr(LiteralIntExprNode &node) = 0;
        virtual void visitLiteralBoolExpr(LiteralBoolExprNode &node) = 0;
        virtual void visitFunctionCallExpr(FunctionCallExprNode &node) = 0;
        virtual void visitVarRefExpr(VarRefExprNode &node) = 0;
        virtual void visitNull(NullNode &node) = 0;
    };
}

#endif