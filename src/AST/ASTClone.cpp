// deep-clone implementations for every concrete AST node
//
// these are gathered in one translation unit (rather than scattered inline across the
// header-only node classes) so the whole clone contract is reviewable in one place and so
// each node header only needs a one-line `clone(...) override` declaration. Node::clone is
// pure-virtual, so a new concrete node that forgets to implement it here fails to compile;
// that compile-time exhaustiveness is exactly what the monomorphizer relies on
//
// conventions used below:
//   cc.shallow(this)  copy-construct a shallow copy (all scalar/token/enum fields) and record
//                     the old->new mapping, then fix up edges/types in place.
//   cc.make<T>(this,) construct a fresh T from ctor args (used when a field is const, e.g. TypeNode)
//   cc.child(ptr)     deep-clone an owned child (recurses through clone())
//   cc.rebind(ptr)    a cross-reference: clone if it was cloned in this subtree, else the original
//   cc.clone_ref(ref) deep-clone an owned NodeReference target, preserving its type tag
//   cc.substitute(t)  run the type through the active TypeSubstitution

#include "AST/ASTClone.h"

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
#include "AST/FunctionDeclNode.h"
#include "AST/ReturnNode.h"
#include "AST/GuardNode.h"
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
#include "AST/ReleaseNode.h"
#include "AST/TemporaryBindExprNode.h"

namespace AST
{

// ---------------------------------------------------------------------------
// leaves - no owned children, no types to substitute. A shallow copy is enough
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
    // concrete instance afterwards. rebind keeps self-recursive calls correct in the meantime
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

Node *StrongExprNode::clone(CloneContext &cc) const
{
    StrongExprNode *c = cc.shallow(this);
    c->operand = cc.child(c->operand);
    return c;
}

Node *NullCoalesceExprNode::clone(CloneContext &cc) const
{
    NullCoalesceExprNode *c = cc.shallow(this);
    c->lhs = cc.child(c->lhs);
    c->rhs = cc.child(c->rhs);
    return c;
}

Node *ChainBaseNode::clone(CloneContext &cc) const
{
    ChainBaseNode *c = cc.shallow(this);
    c->type = cc.substitute(c->type);
    return c;
}

Node *OptionalChainExprNode::clone(CloneContext &cc) const
{
    OptionalChainExprNode *c = cc.shallow(this);
    c->base = cc.child(c->base);

    // the marker **before** the continuation, which reaches it through an ordinary child edge: cloning the
    // other way round would leave the continuation pointing at the template's marker. the same ordering
    // TemporaryBindExprNode's header spells out, and for the same reason
    c->chain_base = cc.child(c->chain_base);
    c->continuation = cc.child(c->continuation);
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

Node *ClosureExprNode::clone(CloneContext &cc) const
{
    ClosureExprNode *c = cc.shallow(this);
    // the declaration is not a child - it hangs off the file root, not off this expression - so it is
    // rebound the way a call's decl is. an instantiated body therefore shares the template's closure
    // body, which is correct: a closure cannot name the enclosing type parameters (see todo/A27)
    c->decl = cc.rebind(c->decl);
    // the captured places are read in the *enclosing* frame, so they are ordinary owned children of this
    // expression. the environment type is not cloned: it is a layout, shared like a ComplexType always is
    for (auto &value : c->captured_values) value = cc.child(value);
    return c;
}

Node *IndirectCallExprNode::clone(CloneContext &cc) const
{
    IndirectCallExprNode *c = cc.shallow(this);
    c->callee = cc.child(c->callee);
    for (auto &arg : c->arguments) arg = cc.child(arg);
    return c;
}

Node *RetainExprNode::clone(CloneContext &cc) const
{
    RetainExprNode *c = cc.shallow(this);
    c->operand = cc.child(c->operand);
    return c;
}

Node *TemporaryBindExprNode::clone(CloneContext &cc) const
{
    TemporaryBindExprNode *c = cc.shallow(this);

    // the temporaries **first**, exactly as FunctionDeclNode clones its parameters before its body: the
    // body and the drops reach them through a VarNode, whose declaration edge is a cc.rebind - and
    // rebind answers the clone only for a declaration the map already holds. cloned in the other order
    // the body would keep pointing at the *original* temporary, which has no alloca in this function,
    // and the failure is a "Variable has no allocation in scope" from a body nobody wrote
    //
    // load-bearing here for the same reason it is in FunctionDeclNode: a temporary hangs off this node
    // rather than off a scope, so ScopeNode::clone's declaration pre-pass never sees it
    for (auto *&temp : c->temporaries) temp = cc.child(temp);

    c->body = cc.child(c->body);

    // always empty in practice, for AssignNode::teardown_old's reason: only a template body is cloned,
    // and AST::OwnershipPass - the only thing that builds one of these - skips a generic body entirely.
    // cloned anyway because clone has to be total, and because the shallow copy above would otherwise
    // leave two nodes owning one drop
    for (auto &drop : c->teardown) drop = cc.clone_ref(drop);

    // no cc.substitute: this node carries no type of its own, result_type() asks the body
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

    // **exactly one of the two states owns the operands.** before AST::OperatorRewriter fires they
    // hang off this node; after it they are the call's arguments and `base` is null. cloning both
    // unconditionally would duplicate the subtree under two parents, which is precisely what the
    // rewriter cleared `base` to prevent
    c->element_call = cc.child(c->element_call);
    c->base = cc.child(c->base);

    for (auto *&index : c->indices) {
        index = cc.child(index);
    }

    return c;
}

Node *ArrayLiteralExprNode::clone(CloneContext &cc) const
{
    ArrayLiteralExprNode *c = cc.shallow(this);

    for (auto *&element : c->elements) {
        element = cc.child(element);
    }

    // a type, so it is substituted like every other. unset in practice today - a call inside an
    // un-instantiated template body never settles, so nothing has typed the literal by the time the
    // body is cloned - but the shallow copy carries it, and a `bound_type` still naming `T` would
    // have the instance building the template's collection
    if (c->bound_type.has_value()) {
        c->bound_type = cc.substitute(*c->bound_type);
    }

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
    // (a parameter or local), otherwise the original (a captured outer variable)
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

    // always null in practice: only a template body is cloned, and AST::OwnershipPass skips a generic
    // body entirely, so no teardown exists yet when the clone happens. cloned anyway because `clone`
    // has to be total - the monomorphizer relies on that - and because the shallow copy above would
    // otherwise leave two assignments pointing at one scope
    c->teardown_old = cc.child(c->teardown_old);

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

Node *ConstDeclNode::clone(CloneContext &cc) const
{
    ConstDeclNode *c = cc.shallow(this);
    c->_type_node = cc.child(c->_type_node);
    c->value = cc.child(c->value);
    return c;
}

Node *ConstRefExprNode::clone(CloneContext &cc) const
{
    // nothing to fix up: every field is the name and where to look for it, and the resolved declaration is
    // deliberately not among them - a clone is resolved by the expander like any other reference
    return cc.shallow(this);
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
    for (auto &drop : c->unwind) drop = cc.clone_ref(drop);
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

Node *GuardNode::clone(CloneContext &cc) const
{
    GuardNode *c = cc.shallow(this);
    c->decl = cc.child(c->decl);
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

Node *LoopControlNode::clone(CloneContext &cc) const
{
    LoopControlNode *c = cc.shallow(this);
    for (auto &drop : c->unwind) drop = cc.clone_ref(drop);
    return c;
}

Node *ForeachNode::clone(CloneContext &cc) const
{
    ForeachNode *c = cc.shallow(this);
    c->source = cc.child(c->source);

    // **the two bindings before the body**, for ScopeNode::clone's own reason: a read inside the body
    // reaches its declaration through cc.rebind, and rebind answers with the *original* for anything the
    // map does not hold yet. cc.child memoizes, so cloning the body next reuses these rather than
    // minting a second pair the body's reads would not be bound to
    c->key = cc.child(c->key);
    c->element = cc.child(c->element);
    c->body = cc.child(c->body);
    return c;
}

Node *ScopeNode::clone(CloneContext &cc) const
{
    // start from a fresh scope (rather than a shallow copy) so the child list and the
    // name->decl lookup maps are rebuilt from clones. Recorded before recursing so child
    // scopes that point back at this one via parent_ptr rebind correctly
    ScopeNode *c = cc.make<ScopeNode>(this);
    c->parent_ptr = cc.rebind(parent_ptr);
    c->is_function_boundary = is_function_boundary;

    // the declarations **first**, ahead of the statements that read them. a read reaches its declaration
    // through cc.rebind, and rebind answers with the *original* for anything the map does not hold yet -
    // so cloning in child order alone, a declaration that sits after a statement reading it leaves that
    // read bound to the *template's* declaration. that is storage in a function nobody wrote, and it
    // fails silently: nothing downstream can tell a legitimate outer-scope reference from this
    //
    // this is what makes a declaration's position in the child list mean nothing here. cc.child answers
    // with the clone it already made, so the loop below reuses these rather than cloning them twice
    for (const auto &ref : children) {
        if (ref.has_type<VarDeclNode>()) {
            cc.child(ref.get_ptr<VarDeclNode>());
        }
    }

    for (const auto &ref : children) {
        c->children.push_back(cc.clone_ref(ref));
    }

    for (const auto &[name, decl] : _declared_variables) c->_declared_variables[name] = cc.rebind(decl);
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

    // parameters first, so the map is populated before the body rebinds its VarNodes to them. the same
    // rule ScopeNode::clone follows for a scope's declarations - and this loop is the only thing that can
    // apply it here, because a parameter is not a *child* of the body scope. it is declared in the
    // argument scope for name resolution and its storage comes from `args` (see Parser::push_implicit_param),
    // so nothing walking the body reaches it
    for (auto &arg : c->args) arg = cc.child(arg);
    c->return_type = cc.child(c->return_type);
    c->body = cc.child(c->body);
    return c;
}

Node *TypeDeclNode::clone(CloneContext &cc) const
{
    // a type declaration is not instantiated: a clone *shares* it. Which ComplexType a generic
    // application means is TypeRegistry::get_or_create_instantiation's answer and only its - struct
    // equality is ComplexType* identity, so a second substituted layout minted here (which is what
    // this used to do) was one type wearing two, unequal to itself across the two paths
    //
    // sound because parse_typedecl refuses a type written where a type parameter is visible, the way
    // parse_funcdecl refuses a nested function - and every body the monomorphizer clones is a generic
    // one - so a declaration reached from here owes no substitution at all. the assert states that
    // invariant where it is relied on rather than where it is enforced
    //
    // returning `this` rather than asserting keeps clone total, which the monomorphizer requires, and
    // is still the right answer if some later path reaches a concrete declaration from a cloned body.
    // the context goes unread for the same reason: nothing is substituted, and nothing is recorded in
    // cc.map either, since rebind falling back to the original is already the answer sharing wants
    (void)cc;

#ifndef NDEBUG
    for (const auto *prop : _properties) {
        assert(!prop->has_type() || !contains_type_param(prop->type()));
    }
#endif

    return const_cast<TypeDeclNode *>(this);
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

};  // namespace AST
