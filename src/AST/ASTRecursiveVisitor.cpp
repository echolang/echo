// default child recursion for the RecursiveVisitor walker. the edges walked here mirror the owned
// children in src/AST/ASTClone.cpp; cross-reference edges (decl pointers) are intentionally skipped.

#include "AST/ASTRecursiveVisitor.h"

#include "AST/ScopeNode.h"
#include "AST/OperatorNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/VarMutNode.h"
#include "AST/TypeNode.h"
#include "AST/TypeCastNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ReturnNode.h"
#include "AST/IfStatementNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/MemberMutNode.h"
#include "AST/NullNode.h"
#include "AST/NamespaceDeclNode.h"
#include "AST/NamespaceNode.h"
#include "AST/AttributeNode.h"
#include "AST/StructNode.h"

namespace AST
{

RecursiveVisitor::~RecursiveVisitor() {}

void RecursiveVisitor::visitScope(ScopeNode &node)
{
    for (auto &ref : node.children) {
        if (ref.has()) {
            ref.node()->accept(*this);
        }
    }
}

void RecursiveVisitor::visitFunctionDecl(FunctionDeclNode &node)
{
    for (auto *arg : node.args) {
        if (arg) arg->accept(*this);
    }
    if (node.body) {
        node.body->accept(*this);
    }
}

void RecursiveVisitor::visitStructDecl(StructDeclNode &node)
{
    for (auto *prop : node.properties()) {
        if (prop) prop->accept(*this);
    }
}

void RecursiveVisitor::visitVarDecl(VarDeclNode &node)
{
    if (node.init_expr) {
        node.init_expr->accept(*this);
    }
}

void RecursiveVisitor::visitVarMut(VarMutNode &node)
{
    if (node.value_expr) {
        node.value_expr->accept(*this);
    }
}

void RecursiveVisitor::visitMemberMut(MemberMutNode &node)
{
    if (node.member_access) node.member_access->accept(*this);
    if (node.value_expr) node.value_expr->accept(*this);
}

void RecursiveVisitor::visitReturn(ReturnNode &node)
{
    if (node.expr) {
        node.expr->accept(*this);
    }
}

void RecursiveVisitor::visitIfStatement(IfStatementNode &node)
{
    if (node.condition) node.condition->accept(*this);
    if (node.if_scope) node.if_scope->accept(*this);
    if (node.else_scope) node.else_scope->accept(*this);
}

void RecursiveVisitor::visitWhileStatement(WhileStatementNode &node)
{
    if (node.condition) node.condition->accept(*this);
    if (node.loop_scope) node.loop_scope->accept(*this);
}

void RecursiveVisitor::visitTypeCast(TypeCastNode &node)
{
    if (node.expr) {
        node.expr->accept(*this);
    }
}

void RecursiveVisitor::visitFunctionCallExpr(FunctionCallExprNode &node)
{
    for (auto *arg : node.arguments) {
        if (arg) arg->accept(*this);
    }
}

void RecursiveVisitor::visitBinaryExpr(BinaryExprNode &node)
{
    if (node.lhs) node.lhs->accept(*this);
    if (node.rhs) node.rhs->accept(*this);
}

void RecursiveVisitor::visitUnaryExpr(UnaryExprNode &node)
{
    if (node.expr) {
        node.expr->accept(*this);
    }
}

void RecursiveVisitor::visitVarPtrExpr(VarPtrExprNode &node)
{
    if (node.var_ref) {
        node.var_ref->accept(*this);
    }
}

void RecursiveVisitor::visitVarRef(VarRefNode &node)
{
    if (node.is_var()) {
        node.get_var().accept(*this);
    }
}

void RecursiveVisitor::visitMemberAccess(MemberAccessNode &node)
{
    auto &base = node.get_base_node();
    if (base.has()) {
        base.node()->accept(*this);
    }
}

// leaves and cross-reference-only nodes: nothing to descend into.
void RecursiveVisitor::visitType(TypeNode &node) {}
void RecursiveVisitor::visitVar(VarNode &node) {}
void RecursiveVisitor::visitLiteralFloatExpr(LiteralFloatExprNode &node) {}
void RecursiveVisitor::visitLiteralIntExpr(LiteralIntExprNode &node) {}
void RecursiveVisitor::visitLiteralBoolExpr(LiteralBoolExprNode &node) {}
void RecursiveVisitor::visitLiteralStringExpr(LiteralStringExprNode &node) {}
void RecursiveVisitor::visitNull(NullNode &node) {}
void RecursiveVisitor::visitOperator(OperatorNode &node) {}
void RecursiveVisitor::visitNamespaceDecl(NamespaceDeclNode &node) {}
void RecursiveVisitor::visitNamespace(NamespaceNode &node) {}
void RecursiveVisitor::visitAttribute(AttributeNode &node) {}

}  // namespace AST
