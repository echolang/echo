// deep-clone implementations for every concrete AST node.
//
// these are gathered in one translation unit (rather than scattered inline across the
// header-only node classes) so the whole clone contract is reviewable in one place and so
// each node header only needs a one-line `clone(...) override` declaration. Node::clone is
// pure-virtual, so a new concrete node that forgets to implement it here fails to compile;
// that compile-time exhaustiveness is exactly what the monomorphizer relies on.
//
// conventions used below:
//   cc.shallow(this)  copy-construct a shallow copy (all scalar/token/enum fields) and record
//                     the old->new mapping, then fix up edges/types in place.
//   cc.make<T>(this,) construct a fresh T from ctor args (used when a field is const, e.g. TypeNode).
//   cc.child(ptr)     deep-clone an owned child (recurses through clone()).
//   cc.rebind(ptr)    a cross-reference: clone if it was cloned in this subtree, else the original.
//   cc.clone_ref(ref) deep-clone an owned NodeReference target, preserving its type tag.
//   cc.substitute(t)  run the type through the active TypeSubstitution.

#include "AST/ASTClone.h"

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
#include "AST/FunctionDeclNode.h"
#include "AST/ReturnNode.h"
#include "AST/IfStatementNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/NullNode.h"
#include "AST/NamespaceDeclNode.h"
#include "AST/NamespaceNode.h"
#include "AST/AttributeNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/ReleaseNode.h"

namespace AST
{

// ---------------------------------------------------------------------------
// leaves - no owned children, no types to substitute. A shallow copy is enough.
// ---------------------------------------------------------------------------

Node *OperatorNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *NullNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *VoidExprNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *LiteralFloatExprNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *LiteralIntExprNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *LiteralBoolExprNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *LiteralStringExprNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *NamespaceDeclNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *NamespaceNode::clone(CloneContext &cc) const { return cc.shallow(this); }

// ---------------------------------------------------------------------------
// types - TypeNode::type is const, so the substituted type must be set at construction.
// ---------------------------------------------------------------------------

Node *TypeNode::clone(CloneContext &cc) const
{
    return type_token.has_value()
        ? cc.make<TypeNode>(this, cc.substitute(type), type_token.value())
        : cc.make<TypeNode>(this, cc.substitute(type));
}

Node *TypeCastNode::clone(CloneContext &cc) const
{
    TypeCastNode *c = cc.shallow(this);
    c->cast_to = cc.substitute(c->cast_to);
    c->expr = cc.child(c->expr);
    return c;
}

// ---------------------------------------------------------------------------
// expressions
// ---------------------------------------------------------------------------

Node *FunctionCallExprNode::clone(CloneContext &cc) const
{
    FunctionCallExprNode *c = cc.shallow(this);
    for (auto &arg : c->arguments) arg = cc.child(arg);
    for (auto &ta : c->explicit_type_args) ta = cc.child(ta);
    // decl points at the (generic) declaration; the monomorphizer repoints it at the
    // concrete instance afterwards. rebind keeps self-recursive calls correct in the meantime.
    c->decl = cc.rebind(c->decl);
    return c;
}

Node *BinaryExprNode::clone(CloneContext &cc) const
{
    BinaryExprNode *c = cc.shallow(this);
    c->op_node = cc.child(c->op_node);
    c->lhs = cc.child(c->lhs);
    c->rhs = cc.child(c->rhs);
    return c;
}

Node *UnaryExprNode::clone(CloneContext &cc) const
{
    UnaryExprNode *c = cc.shallow(this);
    c->expr = cc.child(c->expr);
    return c;
}

Node *AddrOfExprNode::clone(CloneContext &cc) const
{
    AddrOfExprNode *c = cc.shallow(this);
    c->operand = cc.child(c->operand);
    return c;
}

Node *PointerValueNode::clone(CloneContext &cc) const
{
    PointerValueNode *c = cc.shallow(this);
    c->operand = cc.child(c->operand);
    return c;
}

Node *MoveExprNode::clone(CloneContext &cc) const
{
    MoveExprNode *c = cc.shallow(this);
    c->operand = cc.child(c->operand);
    return c;
}

Node *InstanceOfExprNode::clone(CloneContext &cc) const
{
    InstanceOfExprNode *c = cc.shallow(this);
    c->operand = cc.child(c->operand);
    c->queried_type = cc.substitute(c->queried_type);
    return c;
}

Node *RetainExprNode::clone(CloneContext &cc) const
{
    RetainExprNode *c = cc.shallow(this);
    c->operand = cc.child(c->operand);
    return c;
}

Node *ReleaseNode::clone(CloneContext &cc) const
{
    ReleaseNode *c = cc.shallow(this);
    c->target = cc.child(c->target);
    return c;
}

Node *ClassAllocExprNode::clone(CloneContext &cc) const
{
    // the class type is the only edge, and it is a *type* - so it substitutes rather than being
    // rebound. a generic class's constructor carries the self-application `Foo<T>` here, and this is
    // what turns it into `Foo<int32>` in the instance
    ClassAllocExprNode *c = cc.shallow(this);
    c->class_type = cc.substitute(c->class_type);
    return c;
}

Node *IndexExprNode::clone(CloneContext &cc) const
{
    IndexExprNode *c = cc.shallow(this);
    c->base = cc.child(c->base);
    c->index = cc.child(c->index);
    return c;
}

Node *DerefExprNode::clone(CloneContext &cc) const
{
    DerefExprNode *c = cc.shallow(this);
    c->operand = cc.child(c->operand);
    return c;
}

// ---------------------------------------------------------------------------
// variables & references
// ---------------------------------------------------------------------------

Node *VarNode::clone(CloneContext &cc) const
{
    // _decl is a cross-reference: the cloned decl if it lived inside the cloned subtree
    // (a parameter or local), otherwise the original (a captured outer variable).
    if (_token_varname.has_value()) {
        return cc.make<VarNode>(this, cc.rebind(_decl), _token_varname.value());
    }
    return cc.make<VarNode>(this, cc.rebind(_decl));
}

Node *VarRefNode::clone(CloneContext &cc) const
{
    return cc.make<VarRefNode>(this, cc.child(_target_node.get_ptr<VarNode>()));
}

Node *AssignNode::clone(CloneContext &cc) const
{
    AssignNode *c = cc.shallow(this);
    c->target = cc.child(c->target);
    c->value_expr = cc.child(c->value_expr);
    return c;
}

Node *VarDeclNode::clone(CloneContext &cc) const
{
    VarDeclNode *c = cc.shallow(this);
    c->_type_node = cc.child(c->_type_node);
    c->init_expr = cc.child(c->init_expr);
    c->points_to = cc.rebind(c->points_to);
    return c;
}

// ---------------------------------------------------------------------------
// member access / mutation
// ---------------------------------------------------------------------------

Node *MemberAccessNode::clone(CloneContext &cc) const
{
    MemberAccessNode *c = cc.shallow(this);
    c->_base_node = cc.clone_ref(_base_node);
    return c;
}

// ---------------------------------------------------------------------------
// statements & scopes
// ---------------------------------------------------------------------------

Node *ReturnNode::clone(CloneContext &cc) const
{
    ReturnNode *c = cc.shallow(this);
    c->expr = cc.child(c->expr);
    return c;
}

Node *IfStatementNode::clone(CloneContext &cc) const
{
    IfStatementNode *c = cc.shallow(this);
    c->condition = cc.child(c->condition);
    c->if_scope = cc.child(c->if_scope);
    c->else_scope = cc.child(c->else_scope);
    return c;
}

Node *WhileStatementNode::clone(CloneContext &cc) const
{
    WhileStatementNode *c = cc.shallow(this);
    c->condition = cc.child(c->condition);
    c->loop_scope = cc.child(c->loop_scope);
    return c;
}

Node *ScopeNode::clone(CloneContext &cc) const
{
    // start from a fresh scope (rather than a shallow copy) so the child list and the
    // name->decl lookup maps are rebuilt from clones. Recorded before recursing so child
    // scopes that point back at this one via parent_ptr rebind correctly.
    ScopeNode *c = cc.make<ScopeNode>(this);
    c->parent_ptr = cc.rebind(parent_ptr);

    for (const auto &ref : children) {
        c->children.push_back(cc.clone_ref(ref));
    }

    for (const auto &[name, decl] : _declared_variables) c->_declared_variables[name] = cc.rebind(decl);
    for (const auto &[name, decl] : _declared_types) c->_declared_types[name] = cc.rebind(decl);
    for (auto *attr : _attribute_stack) c->_attribute_stack.push_back(cc.rebind(attr));

    return c;
}

// ---------------------------------------------------------------------------
// declarations
// ---------------------------------------------------------------------------

Node *FunctionDeclNode::clone(CloneContext &cc) const
{
    FunctionDeclNode *c = cc.shallow(this);

    // a clone is a concrete instance, never a template. this also has to happen because the
    // shallow copy carried the template's TypeParamDecl pointers, whose owner back-reference
    // still names the template - the declarations themselves stay owned by the registry arena,
    // so dropping the pointers frees nothing and dangles nothing
    c->type_parameters.clear();

    // and with them the owner/own split, for the same reason: cc.shallow copy-constructs, so an
    // instance would otherwise claim inherited parameters it no longer carries. owner_type does
    // stay - an instance is still a member of its owner, and the mangler needs the segment
    c->inherited_type_param_count = 0;

    // likewise the instantiation identity: cc.shallow copy-constructs, so a nested
    // instantiation would otherwise inherit the enclosing instance's type arguments and mangle
    // under its symbol. the monomorphizer sets these on the instance right after cloning
    c->instantiation_args.clear();
    c->template_ref = nullptr;

    // parameters first, so the map is populated before the body rebinds its VarNodes to them.
    for (auto &arg : c->args) arg = cc.child(arg);
    c->return_type = cc.child(c->return_type);
    c->body = cc.child(c->body);
    return c;
}

Node *TypeDeclNode::clone(CloneContext &cc) const
{
    TypeDeclNode *c = cc.shallow(this);

    // the embedded complex type with substituted property types and nothing left that identifies it
    // as a template - ComplexType::substituted_copy owns that rule, so a field added to it survives
    // here by default. Phase 4 reconciles this with the registry's canonical application ComplexType
    // for codegen identity; for now the clone owns its own substituted layout -
    // see todo/A5-reconcile-instantiation-identity.md.
    // no self-pointer to fix up afterwards: value_type() is computed from _complex_type, so the clone
    // answers with its own layout the moment this assignment lands
    c->_complex_type = c->_complex_type.substituted_copy(
        [&cc](const ValueType &type) { return cc.substitute(type); });

    for (auto &prop : c->_properties) prop = cc.child(prop);
    return c;
}

// ---------------------------------------------------------------------------
// attributes
// ---------------------------------------------------------------------------

Node *AttributeNode::clone(CloneContext &cc) const
{
    AttributeNode *c = cc.shallow(this);
    for (auto &expr_ref : c->attribute_exprs) expr_ref = cc.clone_ref(expr_ref);
    return c;
}

}  // namespace AST
