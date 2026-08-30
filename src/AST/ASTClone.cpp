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

#include "AST/ASTConstFold.h"
#include "AST/ASTFile.h"
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
#include "AST/FunctionDeclNode.h"
#include "AST/ReturnNode.h"
#include "AST/GuardNode.h"
#include "AST/IfStatementNode.h"
#include "AST/ConstIfNode.h"
#include "AST/ConstExprNode.h"
#include "AST/WhileStatementNode.h"
#include "AST/ForStatementNode.h"
#include "AST/LoopControlNode.h"
#include "AST/ForeachNode.h"
#include "AST/StaticPropertyExprNode.h"
#include "AST/StringInterpolationNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/NullNode.h"
#include "AST/NamespaceDeclNode.h"
#include "AST/UseDeclNode.h"
#include "AST/NamespaceNode.h"
#include "AST/AttributeNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/ReleaseNode.h"
#include "AST/MatchExprNode.h"
#include "AST/TemporaryBindExprNode.h"

#include <fmt/core.h>

namespace AST
{

// ---------------------------------------------------------------------------
// leaves - no owned children, no types to substitute. A shallow copy is enough
// ---------------------------------------------------------------------------

Node *OperatorNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *NullNode::clone(CloneContext &cc) const
{
    NullNode *c = cc.shallow(this);

    // `return null` of a `T?` binds the template's T? at parse time. without substituting,
    // the instance still holds that type parameter and codegen sees an untyped null at an
    // `int32?` destination
    if (c->bound_type.has_value()) {
        c->bound_type = cc.substitute(*c->bound_type);
    }

    return c;
}
Node *VoidExprNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *LiteralFloatExprNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *LiteralIntExprNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *LiteralBoolExprNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *LiteralStringExprNode::clone(CloneContext &cc) const { return cc.shallow(this); }

Node *StaticPropertyExprNode::clone(CloneContext &cc) const
{
    StaticPropertyExprNode *c = cc.shallow(this);

    // **the owner substitutes, and that is the whole of the clone.** `Box<T>::$count` written
    // inside a generic body has to become `Box<int32>::$count` in the instance, or every
    // instantiation would name the template's storage - which is one global for what are meant to
    // be several. the declaration is *not* rebound: it lives on the template and is shared, which
    // is the same arrangement a method's declaration has
    c->owner = cc.substitute(c->owner);

    return c;
}

Node *StringInterpolationExprNode::clone(CloneContext &cc) const
{
    // the chunks and the specs are plain strings on the node, so the shallow copy carries them; only
    // the hole expressions are owned edges
    StringInterpolationExprNode *c = cc.shallow(this);

    for (auto &hole : c->holes) {
        hole.expr = cc.child(hole.expr);
    }

    return c;
}
Node *NamespaceDeclNode::clone(CloneContext &cc) const { return cc.shallow(this); }
Node *NamespaceNode::clone(CloneContext &cc) const { return cc.shallow(this); }

// ---------------------------------------------------------------------------
// types - TypeNode::type is const, so the substituted type must be set at construction.
// ---------------------------------------------------------------------------

Node *TypeNode::clone(CloneContext &cc) const
{
    TypeNode *c = type_token.has_value()
        ? cc.make<TypeNode>(this, cc.substitute(type), type_token.value())
        : cc.make<TypeNode>(this, cc.substitute(type));

    c->written_names.reserve(written_names.size());
    for (TypeNode *name : written_names) {
        c->written_names.push_back(cc.child(name));
    }

    return c;
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

    // **the owner of a static call is a *type*, so it substitutes like a parameter type does.**
    // cc.shallow copy-constructs, so without this line a `result<T, E>::ok(...)` written inside a
    // generic body keeps the template's `result<T, E>` in every instance - and since the owner is what
    // AST::can_instantiate binds E from, the instantiation stays undecidable forever. Silently: both
    // the monomorphizer's still-generic skip and determine_type_args' undecided arm report nothing,
    // so the call is emitted nowhere and nothing says why
    c->static_owner = cc.substitute(c->static_owner);

    // the same trap for `T(...)`: without this, every instance keeps the template's type parameter
    // and the registry has nothing concrete to look up. silent, for the same two skips
    c->constructed_type = cc.substitute(c->constructed_type);
    return c;
}

Node *FunctionRefExprNode::clone(CloneContext &cc) const
{
    FunctionRefExprNode *c = cc.shallow(this);
    c->decl = cc.rebind(c->decl);
    c->static_owner = cc.substitute(c->static_owner);
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

    // the copy the present arm is given, when there is one. an instance whose payload became
    // owning gets its own from AST::OwnershipPass, but a template that already had one must not
    // hand the instance the template's nodes
    c->present_value = cc.child(c->present_value);

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

    // the stored result type, substituted like the marker's beside it. AST::OperatorRewriter refreshes it
    // every round anyway, but a clone that carried the template's `T?` into an instance would be read once
    // before that happens
    c->result = cc.substitute(c->result);
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

    // a fresh environment per instance: the template's layout still names T, and two instances
    // of `spawn<int32>` must not share one ComplexType whose properties were substituted in
    // place. the function's identity is the enclosing instance's `instantiation_args`, not
    // this name — same-shape captures still need that stamp to mangle apart
    ComplexType *new_env = nullptr;

    if (environment_type != nullptr) {
        std::string env_name = environment_type->name.value_or("env");

        for (size_t i = 0; i < environment_type->property_count(); i++) {
            env_name += "." + cc.substitute(environment_type->get_property(i).type).get_mangled_name();
        }

        new_env = cc.registry.create_anonymous_type(
            env_name, ComplexTypeKind::t_class, environment_type->ast_namespace);

        for (size_t i = 0; i < environment_type->property_count(); i++) {
            const ComplexType::Property &prop = environment_type->get_property(i);
            new_env->add_property(prop.name, cc.substitute(prop.type), prop.visibility);
        }

        c->environment_type = new_env;
    }

    // the declaration is not a child of the expression - it hangs off the file root - so the
    // clone has to be asked for here. sharing it would leave every instance calling the
    // template's body, whose `$__env` is still typed with T
    if (decl != nullptr) {
        c->decl = cc.child(decl);
        if (new_env != nullptr && !c->decl->args.empty() && c->decl->args[0] != nullptr) {
            auto &env_type = cc.nodes.emplace_back<TypeNode>(ValueType::make_class(new_env));
            c->decl->args[0]->set_type_node(&env_type);
        }
    }

    // the captured places are read in the *enclosing* frame, so they are ordinary owned children of this
    // expression. capture_list is tokens, copied by shallow
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

Node *MatchExprNode::clone(CloneContext &cc) const
{
    MatchExprNode *c = cc.shallow(this);

    // **the subject first**, exactly as TemporaryBindExprNode clones its temporaries before its body
    // and for its reason: every arm reaches it through a VarNode whose declaration edge is a cc.rebind,
    // and rebind answers the clone only for a declaration the map already holds. cloned in the other
    // order the arms would keep pointing at the *original* declaration, which has no alloca in this
    // function - a "Variable has no allocation in scope" out of a body nobody wrote
    //
    // load-bearing here for the same reason it is there: the subject hangs off this node rather than
    // off a scope, so ScopeNode::clone's declaration pre-pass never sees it
    c->subject = cc.child(c->subject);

    for (Arm &arm : c->arms) {
        // the written owner is a type node, so it substitutes with the rest - `match` inside a generic
        // body may write `result<T, E>::ok`, and an instance of it means its own arguments
        arm.owner = cc.child(arm.owner);

        // the scope before the value, which is the order they are reached in: the bindings are the
        // scope's own children, and the value reads them
        arm.scope = cc.child(arm.scope);
        arm.value = cc.child(arm.value);
    }

    // the unified arm type is a type of this node's own, unlike TemporaryBindExprNode's - so unlike
    // that one it needs substituting, or `match` over a generic enum answers the template's type in
    // every instance
    c->result = cc.substitute(c->result);

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
    // body entirely, so no teardown or bind exists yet when the clone happens. cloned anyway because
    // `clone` has to be total - the monomorphizer relies on that - and because the shallow copy above
    // would otherwise leave two assignments pointing at one scope
    c->target_bind = cc.child(c->target_bind);
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

// **clone totality is the feature here, not a formality.** the only `const if` that matters today lives
// in a generic body - stdlib/core/array.eco's - and the *only* way it ever reaches a concrete `T` is by
// being cloned into an instance. a missing edge here would leave an arm behind in the template
Node *ConstIfNode::clone(CloneContext &cc) const
{
    // clone the condition first so type arguments substitute. if it folds, only the taken arm is
    // cloned - the discarded one never exists in the instance, so OperatorRewriter cannot mint
    // into it and TypeLowering cannot emit it. this stays a ConstIfNode: ConstFolding is the splicer
    ConstIfNode *c = cc.shallow(this);
    c->condition = cc.child(condition);

    if (c->condition != nullptr) {
        const ConstFoldResult folded = const_fold(c->condition);

        if (folded.is_bool()) {
            ScopeNode *taken = taken_const_if_arm(*this, folded.as_bool());
            c->if_scope = taken == if_scope ? cc.child(if_scope) : nullptr;
            c->else_scope = taken == else_scope ? cc.child(else_scope) : nullptr;
            return c;
        }
    }

    c->if_scope = cc.child(if_scope);
    c->else_scope = cc.child(else_scope);
    return c;
}

Node *ConstExprNode::clone(CloneContext &cc) const
{
    ConstExprNode *c = cc.shallow(this);

    c->operand = cc.child(c->operand);

    return c;
}

Node *GuardNode::clone(CloneContext &cc) const
{
    GuardNode *c = cc.shallow(this);
    c->decl = cc.child(c->decl);

    // the copy the binding is given, when there is one. an instance whose payload became owning gets its
    // own from AST::OwnershipPass, but a template that already had one must not hand the instance the
    // template's nodes - the same rule every owned edge here follows
    c->bound_value = cc.child(c->bound_value);

    // the `has_value()` call, on the same terms and for the same reason
    c->presence_test = cc.child(c->presence_test);

    // **before the rebind below**, because the else scope is what owns the failure declaration: it was
    // seeded as that scope's first child, so cloning the scope clones the declaration and `cc.rebind`
    // then finds it
    c->else_scope = cc.child(c->else_scope);

    // **a cross-reference, not an owned edge.** `cc.child` here would be a *second* clone of a
    // declaration the else scope already cloned - two slots with half the reads bound to each, which is
    // the failure mode a rebind exists to prevent
    c->failure = cc.rebind(c->failure);

    return c;
}

Node *WhileStatementNode::clone(CloneContext &cc) const
{
    WhileStatementNode *c = cc.shallow(this);
    c->condition = cc.child(c->condition);
    c->loop_scope = cc.child(c->loop_scope);
    return c;
}

Node *ForStatementNode::clone(CloneContext &cc) const
{
    ForStatementNode *c = cc.shallow(this);
    c->condition = cc.child(c->condition);
    c->loop_scope = cc.child(c->loop_scope);
    c->step = cc.child(c->step);
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

    // **not a shallow copy, so every flag is copied by hand and a new one is silent if forgotten.**
    // this one is `unsafe { }`: a generic body is cloned per instantiation, so without this line
    // `mem::alloc<T>` keeps its promise in the template and loses it in every instance - which reads
    // as the standard library refusing to compile against its own rule
    c->is_unsafe = is_unsafe;

    // and the opening brace, for the same reason: a DILexicalBlock is placed at it, so an instantiation
    // that lost it would describe every block of its body as starting at the function's own line
    // emplace rather than assign: TokenReference holds a reference, so optional's copy assignment is
    // deleted and only in-place construction is available
    if (token_brace.has_value()) {
        c->token_brace.emplace(token_brace.value());
    }

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
    c->instances.clear();

    // a clone is a new region. cc.shallow copy-constructs, so an instance of an already-owned
    // template would otherwise start t_owned and skip the ownership walk
    c->region_state = RegionState::t_open;

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
    // equality is ComplexType* identity, so a second substituted layout minted here would be one
    // type wearing two, unequal to itself across the two paths
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

// an attribute value is a plain value holding no node edges, so the shallow copy is nearly the whole
// clone - unlike every other node here, which owes its children a visit. `scope_owner` is the one edge,
// and it is a cross-reference: a clone of a scoped attribute belongs to the clone of its owner when both
// travelled together, and to the original otherwise, which is exactly what rebind answers
Node *AttributeNode::clone(CloneContext &cc) const
{
    AttributeNode *c = cc.shallow(this);
    c->scope_owner = cc.rebind(scope_owner);
    return c;
}

void publish_cloned_closures(
    Node &root,
    ScopeNode *into,
    FunctionDeclNode *enclosing_template,
    const std::vector<ValueType> &instantiation_args
)
{
    struct Publisher : RecursiveVisitor
    {
        ScopeNode *into = nullptr;
        FunctionDeclNode *tmpl = nullptr;
        std::vector<ValueType> args;

        void visit_closure_expr(ClosureExprNode &node) override {
            if (FunctionDeclNode *decl = node.decl) {
                decl->template_ref = tmpl;
                decl->instantiation_args = args;

                if (into != nullptr) {
                    bool already = false;
                    for (const auto &child : into->children) {
                        if (child.node() == decl) {
                            already = true;
                            break;
                        }
                    }

                    if (!already) {
                        into->add_funcdecl(*decl);
                    }
                }

                // the declaration hangs off the file root, so RecursiveVisitor will not
                // descend into it from the expression. nested closures live in that body
                if (decl->body != nullptr) {
                    decl->body->accept(*this);
                }
            }

            RecursiveVisitor::visit_closure_expr(node);
        }
    };

    Publisher publisher;
    publisher.into = into;
    publisher.tmpl = enclosing_template;
    publisher.args = instantiation_args;
    root.accept(publisher);
}

void publish_cloned_closures(
    Node &root,
    File *file,
    FunctionDeclNode *enclosing_template,
    const std::vector<ValueType> &instantiation_args
)
{
    publish_cloned_closures(
        root, file != nullptr ? file->root : nullptr, enclosing_template, instantiation_args);
}

ExprNode *clone_sharing_closures(ExprNode *recipe, CloneContext &cc)
{
    if (recipe == nullptr) {
        return nullptr;
    }

    struct RebindClosures : RecursiveVisitor
    {
        CloneContext *cc = nullptr;

        void visit_closure_expr(ClosureExprNode &node) override {
            if (node.decl != nullptr) {
                cc->map[node.decl] = node.decl;
            }

            RecursiveVisitor::visit_closure_expr(node);
        }
    };

    RebindClosures rebind;
    rebind.cc = &cc;
    recipe->accept(rebind);

    return cc.child(recipe);
}

};  // namespace AST
