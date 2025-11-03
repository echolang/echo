// default child recursion for the RecursiveVisitor walker. the edges walked here mirror the owned
// children in src/AST/ASTClone.cpp; cross-reference edges (decl pointers) are intentionally skipped.
//
// **every descent below goes through one of the edge helpers**, never through a bare `accept()`. that
// is what makes the walk rewritable by a subclass without the subclass owning a second copy of this
// list - and the helper it goes through is also the statement of whether the position wants a place or
// a read, which is a fact about the node and belongs here rather than in whichever pass first needed it

#include "AST/ASTRecursiveVisitor.h"

#include "AST/ScopeNode.h"
#include "AST/OperatorNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/VarDeclNode.h"
#include "AST/ConstDeclNode.h"
#include "AST/ConstRefExprNode.h"
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

// ---- the edge seams -------------------------------------------------------------------------

ExprNode *RecursiveVisitor::rewrite_value_edge(ExprNode *expr)
{
    if (expr != nullptr) {
        expr->accept(*this);
    }

    return expr;
}

ExprNode *RecursiveVisitor::rewrite_place_edge(ExprNode *expr)
{
    // a read-only walk cannot tell the two apart, and neither can a rewriter that does not care -
    // routing through the value form means such a pass overrides one method and gets both
    return rewrite_value_edge(expr);
}

void RecursiveVisitor::value_edge(ExprNode *&edge)
{
    edge = rewrite_value_edge(edge);
}

void RecursiveVisitor::place_edge(ExprNode *&edge)
{
    edge = rewrite_place_edge(edge);
}

void RecursiveVisitor::value_edges(std::vector<ExprNode *> &edges)
{
    for (auto *&edge : edges) {
        value_edge(edge);
    }
}

void RecursiveVisitor::value_edge(NodeReference &edge)
{
    rewrite_ref_edge(edge, false);
}

void RecursiveVisitor::place_edge(NodeReference &edge)
{
    rewrite_ref_edge(edge, true);
}

void RecursiveVisitor::rewrite_ref_edge(NodeReference &edge, bool as_place)
{
    if (!edge.has()) {
        return;
    }

    // a reference position may legitimately hold something that is not an expression - a scope's
    // children are the obvious case, and a `->` base can be one too. asked once, here, so a caller
    // cannot get it right in one place and wrong in the next
    if (!edge.is_expression_node()) {
        statement_edge(edge.node());
        return;
    }

    auto *expr = edge.unsafe_ptr<ExprNode>();
    ExprNode *replacement = as_place ? rewrite_place_edge(expr) : rewrite_value_edge(expr);

    if (replacement != expr) {
        edge = replacement != nullptr
            ? NodeReference(static_cast<Node *>(replacement)->get_node_type(), replacement)
            : make_void_ref();
    }
}

void RecursiveVisitor::statement_edge(Node *node)
{
    if (node != nullptr) {
        node->accept(*this);
    }
}

void RecursiveVisitor::statement_edges(NodeReferenceList &edges)
{
    for (auto &ref : edges) {
        statement_edge(ref.node());
    }
}

// ---- the edges themselves -------------------------------------------------------------------

void RecursiveVisitor::visitScope(ScopeNode &node)
{
    // **indexed, and the edge is re-read after the descent.** a subclass may splice statements in
    // around the one being walked (AST::OperatorRewriter's array-literal expansion does) or reseat it
    // (AST::ForeachLowering's lowering does), and a range-for over `children` would be walking a
    // vector whose storage had just moved
    for (size_t i = 0; i < node.children.size(); i++) {
        statement_edge(node.children[i].node());
    }
}

void RecursiveVisitor::visitFunctionDecl(FunctionDeclNode &node)
{
    for (auto *arg : node.args) {
        statement_edge(arg);
    }

    statement_edge(node.body);
}

void RecursiveVisitor::visit_type_decl(TypeDeclNode &node)
{
    for (auto *prop : node.properties()) {
        statement_edge(prop);
    }
}

void RecursiveVisitor::visitVarDecl(VarDeclNode &node)
{
    value_edge(node.init_expr);
}

void RecursiveVisitor::visit_const_decl(ConstDeclNode &node)
{
    value_edge(node.value);
}

void RecursiveVisitor::visit_assign(AssignNode &node)
{
    value_edge(node.target);
    value_edge(node.value_expr);

    // the old value's teardown is part of the statement, so it is part of the walk: this is what lets
    // AST::TypeChecker validate the destructor calls AST::OwnershipPass inserted here, exactly as it
    // validates the ones sitting in a scope's children
    statement_edge(node.teardown_old);
}

void RecursiveVisitor::visitReturn(ReturnNode &node)
{
    value_edge(node.expr);

    // the drops this return owes, which live on the node rather than ahead of it
    statement_edges(node.unwind);
}

void RecursiveVisitor::visitIfStatement(IfStatementNode &node)
{
    value_edge(node.condition);
    statement_edge(node.if_scope);
    statement_edge(node.else_scope);
}

void RecursiveVisitor::visitWhileStatement(WhileStatementNode &node)
{
    value_edge(node.condition);
    statement_edge(node.loop_scope);
}

void RecursiveVisitor::visit_loop_control(LoopControlNode &node)
{
    // the drops this exit owes, which live on the node exactly as a return's do. without this descent
    // AST::TypeChecker never validates them - and "every drop is an ordinary call node in the tree" is
    // the whole reason they are nodes rather than a codegen side effect
    statement_edges(node.unwind);
}

void RecursiveVisitor::visit_foreach(ForeachNode &node)
{
    // the source, then the two bindings, then the body - the order they run in, and the order the
    // clone builds them in. the bindings are `body->children[0..1]` as well, so the body walk reaches
    // them a second time; harmless for every reader of this visitor, and the explicit edges are what
    // let AST::is_never_written find them without knowing that
    value_edge(node.source);
    statement_edge(node.key);
    statement_edge(node.element);
    statement_edge(node.body);
}

void RecursiveVisitor::visitTypeCast(TypeCastNode &node)
{
    value_edge(node.expr);
}

void RecursiveVisitor::visitFunctionCallExpr(FunctionCallExprNode &node)
{
    value_edges(node.arguments);
}

void RecursiveVisitor::visitBinaryExpr(BinaryExprNode &node)
{
    value_edge(node.lhs);
    value_edge(node.rhs);
}

void RecursiveVisitor::visitUnaryExpr(UnaryExprNode &node)
{
    value_edge(node.expr);
}

void RecursiveVisitor::visit_addr_of_expr(AddrOfExprNode &node)
{
    // "no transparency peeling": the operand of `&` is the place whose address is being taken
    place_edge(node.operand);
}

void RecursiveVisitor::visit_deref_expr(DerefExprNode &node)
{
    // the operand names the pointer being read through - this node *is* the read, so the operand
    // below it is not one
    place_edge(node.operand);
}

void RecursiveVisitor::visit_pointer_value(PointerValueNode &node)
{
    // `:$` exists precisely to say "the place, not the value at it"
    place_edge(node.operand);
}

void RecursiveVisitor::visit_move_expr(MoveExprNode &node)
{
    value_edge(node.operand);
}

void RecursiveVisitor::visit_const_ref(ConstRefExprNode &node)
{
    // a leaf: `decl` is a cross-reference into a declaration this walk does not own, and the initializer it
    // points at is walked in its own right - once, by AST::ConstantExpander, rather than once per use site
}

void RecursiveVisitor::visit_class_alloc_expr(ClassAllocExprNode &node)
{
    // a leaf: the allocation has no operand, only the class type it was synthesized for
}

void RecursiveVisitor::visit_retain_expr(RetainExprNode &node)
{
    value_edge(node.operand);
}

void RecursiveVisitor::visit_strong_expr(StrongExprNode &node)
{
    value_edge(node.operand);
}

void RecursiveVisitor::visit_guard(GuardNode &node)
{
    // the declaration first, then the else arm. that order is the order they run in, and it matters to
    // every pass that carries state down the walk - the ownership pass's moved-from set most of all,
    // since the else arm may move out of something the initializer read
    //
    // **the declaration is reachable only from here.** it is not a child of the enclosing scope, only
    // its *name* is registered there - so a pass that walks statements and has no arm for this node
    // never sees the initializer at all. that was two silent bugs before this walk was shared
    statement_edge(node.decl);
    statement_edge(node.else_scope);
}

void RecursiveVisitor::visit_null_coalesce(NullCoalesceExprNode &node)
{
    value_edge(node.lhs);
    value_edge(node.rhs);
}

void RecursiveVisitor::visit_optional_chain(OptionalChainExprNode &node)
{
    // the base, then the continuation - the order they run in. the continuation is rooted at the chain
    // base marker, which is a leaf, so the base is not walked twice
    value_edge(node.base);
    value_edge(node.continuation);
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
    value_edges(node.captured_values);
}

void RecursiveVisitor::visit_indirect_call_expr(IndirectCallExprNode &node)
{
    value_edge(node.callee);
    value_edges(node.arguments);
}

void RecursiveVisitor::visit_instanceof_expr(InstanceOfExprNode &node)
{
    value_edge(node.operand);
}

// the temporaries first, then the body, then the drops - the order they run in, and the order
// AST::TypeChecker has to see them in: a drop names a destructor whose call it validates like any
// other, and a body that reads out of a temporary declared after it would read a declaration this
// visitor had not reached yet
void RecursiveVisitor::visit_temporary_bind(TemporaryBindExprNode &node)
{
    for (auto *temp : node.temporaries) {
        statement_edge(temp);
    }

    value_edge(node.body);
    statement_edges(node.teardown);
}

void RecursiveVisitor::visit_release(ReleaseNode &node)
{
    // the target is the place whose slot is released, read by codegen itself - a release never wants
    // the value in it. AST::PointerAdjuster relies on this being a place edge rather than a read
    place_edge(node.target);
}

void RecursiveVisitor::visit_index_expr(IndexExprNode &node)
{
    statement_edge(node.element_call);

    // the base is wanted as the address it holds - `$p:$[1]` offsets from $p's address, it does not
    // read through it first
    place_edge(node.base);
    value_edges(node.indices);
}

void RecursiveVisitor::visit_array_literal_expr(ArrayLiteralExprNode &node)
{
    value_edges(node.elements);
}

void RecursiveVisitor::visitVarRef(VarRefNode &node)
{
    if (node.is_var()) {
        node.get_var().accept(*this);
    }
}

void RecursiveVisitor::visitMemberAccess(MemberAccessNode &node)
{
    // the base is wanted as a place; `->` reaching through a pointer is gen_place's job
    place_edge(node.get_base_node());
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
