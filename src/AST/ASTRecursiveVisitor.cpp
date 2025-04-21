// default child recursion for the RecursiveVisitor walker. the edges walked here mirror the owned
// children in src/AST/ASTClone.cpp; cross-reference edges (decl pointers) are intentionally skipped.

#include "AST/ASTRecursiveVisitor.h"

#include "AST/ScopeNode.h"
#include "AST/OperatorNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"
#include "AST/AssignNode.h"
#include "AST/TypeNode.h"
#include "AST/TypeCastNode.h"
#include "AST/ExprNode.h"
#include "AST/GuardNode.h"
#include "AST/ReleaseNode.h"
#include "AST/TemporaryBindExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ReturnNode.h"
#include "AST/IfStatementNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/LoopControlNode.h"
#include "AST/ForeachNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/NullNode.h"
#include "AST/NamespaceDeclNode.h"
#include "AST/NamespaceNode.h"
#include "AST/AttributeNode.h"
#include "AST/TypeDeclNode.h"

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

void RecursiveVisitor::visit_type_decl(TypeDeclNode &node)
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

void RecursiveVisitor::visit_assign(AssignNode &node)
{
    if (node.target) node.target->accept(*this);
    if (node.value_expr) node.value_expr->accept(*this);

    // the old value's teardown is part of the statement, so it is part of the walk: this is what lets
    // AST::TypeChecker validate the destructor calls AST::OwnershipPass inserted here, exactly as it
    // validates the ones sitting in a scope's children
    if (node.teardown_old) node.teardown_old->accept(*this);
}

void RecursiveVisitor::visitReturn(ReturnNode &node)
{
    if (node.expr) {
        node.expr->accept(*this);
    }

    // the drops this return owes, which live on the node rather than ahead of it
    for (auto &drop : node.unwind) {
        if (drop.has()) drop.node()->accept(*this);
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

void RecursiveVisitor::visit_loop_control(LoopControlNode &node)
{
    // the drops this exit owes, which live on the node exactly as a return's do. without this descent
    // AST::TypeChecker never validates them - and "every drop is an ordinary call node in the tree" is
    // the whole reason they are nodes rather than a codegen side effect
    for (auto &drop : node.unwind) {
        if (drop.has()) drop.node()->accept(*this);
    }
}

void RecursiveVisitor::visit_foreach(ForeachNode &node)
{
    // the source, then the two bindings, then the body - the order they run in, and the order the
    // clone builds them in. the bindings are `body->children[0..1]` as well, so the body walk reaches
    // them a second time; harmless for every reader of this visitor, and the explicit edges are what
    // let AST::is_never_written find them without knowing that
    if (node.source) node.source->accept(*this);
    if (node.key) node.key->accept(*this);
    if (node.element) node.element->accept(*this);
    if (node.body) node.body->accept(*this);
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

void RecursiveVisitor::visit_addr_of_expr(AddrOfExprNode &node)
{
    if (node.operand) node.operand->accept(*this);
}

void RecursiveVisitor::visit_deref_expr(DerefExprNode &node)
{
    if (node.operand) node.operand->accept(*this);
}

void RecursiveVisitor::visit_pointer_value(PointerValueNode &node)
{
    if (node.operand) node.operand->accept(*this);
}

void RecursiveVisitor::visit_move_expr(MoveExprNode &node)
{
    if (node.operand) node.operand->accept(*this);
}

void RecursiveVisitor::visit_class_alloc_expr(ClassAllocExprNode &node)
{
    // a leaf: the allocation has no operand, only the class type it was synthesized for
}

void RecursiveVisitor::visit_retain_expr(RetainExprNode &node)
{
    if (node.operand) node.operand->accept(*this);
}

void RecursiveVisitor::visit_strong_expr(StrongExprNode &node)
{
    if (node.operand) node.operand->accept(*this);
}

void RecursiveVisitor::visit_guard(GuardNode &node)
{
    // the declaration first, then the else arm. that order is the order they run in, and it matters to
    // every pass that carries state down the walk - the ownership pass's moved-from set most of all,
    // since the else arm may move out of something the initializer read
    if (node.decl) node.decl->accept(*this);
    if (node.else_scope) node.else_scope->accept(*this);
}

void RecursiveVisitor::visit_null_coalesce(NullCoalesceExprNode &node)
{
    if (node.lhs) node.lhs->accept(*this);
    if (node.rhs) node.rhs->accept(*this);
}

void RecursiveVisitor::visit_optional_chain(OptionalChainExprNode &node)
{
    // the base, then the continuation - the order they run in. the continuation is rooted at the chain
    // base marker, which is a leaf, so the base is not walked twice
    if (node.base) node.base->accept(*this);
    if (node.continuation) node.continuation->accept(*this);
}

void RecursiveVisitor::visit_chain_base(ChainBaseNode &node)
{
    // a leaf: it stands for a value the enclosing chain already evaluated, and has no edge to it
}

void RecursiveVisitor::visit_closure_expr(ClosureExprNode &node)
{
    // the *environment* is a child of this expression and is walked; the body is not. the declaration
    // hangs off the file root, so every pass that walks declarations reaches it there exactly once -
    // descending into it here would process it twice
    for (auto *value : node.captured_values) {
        if (value) value->accept(*this);
    }
}

void RecursiveVisitor::visit_indirect_call_expr(IndirectCallExprNode &node)
{
    if (node.callee) node.callee->accept(*this);
    for (auto *arg : node.arguments) {
        if (arg) arg->accept(*this);
    }
}

void RecursiveVisitor::visit_instanceof_expr(InstanceOfExprNode &node)
{
    if (node.operand) node.operand->accept(*this);
}

// the temporaries first, then the body, then the drops - the order they run in, and the order
// AST::TypeChecker has to see them in: a drop names a destructor whose call it validates like any
// other, and a body that reads out of a temporary declared after it would read a declaration this
// visitor had not reached yet
void RecursiveVisitor::visit_temporary_bind(TemporaryBindExprNode &node)
{
    for (auto *temp : node.temporaries) {
        if (temp) temp->accept(*this);
    }

    if (node.body) node.body->accept(*this);

    for (auto &drop : node.teardown) {
        if (drop.has()) drop.node()->accept(*this);
    }
}

void RecursiveVisitor::visit_release(ReleaseNode &node)
{
    if (node.target) node.target->accept(*this);
}

void RecursiveVisitor::visit_index_expr(IndexExprNode &node)
{
    if (node.element_call) node.element_call->accept(*this);
    if (node.base) node.base->accept(*this);
    for (auto *index : node.indices) {
        if (index) index->accept(*this);
    }
}

void RecursiveVisitor::visit_array_literal_expr(ArrayLiteralExprNode &node)
{
    for (auto *element : node.elements) {
        if (element) element->accept(*this);
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

// leaves and cross-reference-only nodes: nothing to descend into
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

};  // namespace AST
