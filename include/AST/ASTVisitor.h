#ifndef ASTVISITOR_H
#define ASTVISITOR_H

#pragma once

namespace AST 
{
    class ScopeNode;
    class TypeNode;
    class VarDeclNode;
    class LiteralFloatExprNode;
    class LiteralIntExprNode;
    class LiteralBoolExprNode;

    class Visitor
    {
    public:
        virtual ~Visitor();

        virtual void visitScope(ScopeNode &node) = 0;
        virtual void visitType(TypeNode &node) = 0;
        virtual void visitVarDecl(VarDeclNode &node) = 0;
        virtual void visitLiteralFloatExpr(LiteralFloatExprNode &node) = 0;
        virtual void visitLiteralIntExpr(LiteralIntExprNode &node) = 0;
        virtual void visitLiteralBoolExpr(LiteralBoolExprNode &node) = 0;
    };
}

#endif