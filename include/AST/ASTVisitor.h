#ifndef ASTVISITOR_H
#define ASTVISITOR_H

#pragma once

namespace AST
{
    class ScopeNode;
    class TypeNode;
    class TypeCastNode;
    class VarDeclNode;
    class ConstDeclNode;
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
    class ConstRefExprNode;
    class ClassAllocExprNode;
    class RetainExprNode;
    class GuardNode;
    class StrongExprNode;
    class NullCoalesceExprNode;
    class OptionalChainExprNode;
    class ChainBaseNode;
    class ClosureExprNode;
    class IndirectCallExprNode;
    class InstanceOfExprNode;
    class TemporaryBindExprNode;
    class ReleaseNode;
    class IndexExprNode;
    class BinaryExprNode;
    class UnaryExprNode;
    class NullNode;
    class OperatorNode;
    class FunctionDeclNode;
    class ReturnNode;
    class IfStatementNode;
    class ConstIfNode;
    class ConstExprNode;
    class WhileStatementNode;
    class LoopControlNode;
    class ForeachNode;
    class AssignNode;
    class NamespaceDeclNode;
    class NamespaceNode;
    class AttributeNode;
    class TypeDeclNode;
    class MemberAccessNode;
    class VarNode;
    class ArrayLiteralExprNode;

    class Visitor
    {
    public:
        virtual ~Visitor();

        virtual void visitScope(ScopeNode &node) = 0;
        virtual void visitType(TypeNode &node) = 0;
        virtual void visitTypeCast(TypeCastNode &node) = 0;
        virtual void visitVarDecl(VarDeclNode &node) = 0;
        virtual void visit_const_decl(ConstDeclNode &node) = 0;
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
        virtual void visit_const_ref(ConstRefExprNode &node) = 0;
        virtual void visit_class_alloc_expr(ClassAllocExprNode &node) = 0;
        virtual void visit_retain_expr(RetainExprNode &node) = 0;
        virtual void visit_strong_expr(StrongExprNode &node) = 0;
        virtual void visit_guard(GuardNode &node) = 0;
        virtual void visit_null_coalesce(NullCoalesceExprNode &node) = 0;
        virtual void visit_optional_chain(OptionalChainExprNode &node) = 0;
        virtual void visit_chain_base(ChainBaseNode &node) = 0;
        virtual void visit_closure_expr(ClosureExprNode &node) = 0;
        virtual void visit_indirect_call_expr(IndirectCallExprNode &node) = 0;
        virtual void visit_instanceof_expr(InstanceOfExprNode &node) = 0;
        virtual void visit_temporary_bind(TemporaryBindExprNode &node) = 0;
        virtual void visit_release(ReleaseNode &node) = 0;
        virtual void visit_index_expr(IndexExprNode &node) = 0;
        virtual void visit_array_literal_expr(ArrayLiteralExprNode &node) = 0;
        virtual void visitBinaryExpr(BinaryExprNode &node) = 0;
        virtual void visitUnaryExpr(UnaryExprNode &node) = 0;
        virtual void visitNull(NullNode &node) = 0;
        virtual void visitOperator(OperatorNode &node) = 0;
        virtual void visitFunctionDecl(FunctionDeclNode &node) = 0;
        virtual void visitReturn(ReturnNode &node) = 0;
        virtual void visitIfStatement(IfStatementNode &node) = 0;
        virtual void visit_const_if(ConstIfNode &node) = 0;
        virtual void visit_const_expr(ConstExprNode &node) = 0;
        virtual void visitWhileStatement(WhileStatementNode &node) = 0;
        virtual void visit_loop_control(LoopControlNode &node) = 0;
        virtual void visit_foreach(ForeachNode &node) = 0;
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
