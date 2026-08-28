#include "AST/ASTConstructor.h"

#include "AST/ASTClone.h"
#include "AST/ASTControlFlow.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTModule.h"
#include "AST/ASTNode.h"
#include "AST/ASTRecursiveVisitor.h"
#include "AST/AssignNode.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/ReturnNode.h"
#include "AST/ScopeNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"

#include <cassert>
#include <vector>

AST::VarDeclNode &AST::declare_constructor_this(
    AST::Module &module,
    AST::TypeNode &self_type,
    const TokenReference &at
)
{
    auto &decl = module.nodes.emplace_back<AST::VarDeclNode>(
        module.make_virtual_token("$this", Token::Type::t_varname, at), &self_type);

    // **the one place a `$this` is `out`.** the storage arrives holding nothing and the body owes it
    // an initialized value, which is the whole of why a constructor may write every property without
    // first destroying what was there.
    //
    // it goes on the declaration and not on a parameter because this `$this` *is* a local - a
    // constructor has no receiver argument, so AST::access_effect_of has nothing to answer for at
    // args[0] and deliberately does not pretend to
    decl.access_effect = AST::AccessEffect::t_out;

    // the one fork between the two storage classes, and the only thing this function decides. a
    // struct's slot is already there when the frame is entered; a class's handle names a block that
    // has to be made, so the declaration carries the allocation as its initializer
    if (self_type.type.is_class()) {
        decl.init_expr = &module.nodes.emplace_back<AST::ClassAllocExprNode>(self_type.type, at);
    }

    return decl;
}

void AST::close_constructor_body(
    AST::Module &module,
    AST::FunctionDeclNode &decl,
    AST::VarDeclNode &this_decl
)
{
    if (decl.body == nullptr || AST::scope_always_leaves_function(*decl.body)) {
        return;
    }

    auto &var = module.nodes.emplace_back<AST::VarNode>(&this_decl);
    auto &read = module.nodes.emplace_back<AST::VarRefNode>(&var);
    auto &ret = module.nodes.emplace_back<AST::ReturnNode>(&read);

    decl.body->children.push_back(AST::make_ref(ret));
}

AST::ExprNode *AST::make_member_place(
    AST::Module &module,
    AST::VarDeclNode &local,
    const std::string &name,
    const TokenReference &at
)
{
    return &module.nodes.emplace_back<AST::MemberAccessNode>(
        AST::make_ref(AST::local_place(module, local)),
        module.make_virtual_token(name, Token::Type::t_identifier, at));
}

AST::AssignNode &AST::seat_property_from_value(
    AST::Module &module,
    AST::VarDeclNode &self,
    const AST::VarDeclNode &property,
    AST::ExprNode *value,
    const TokenReference &at
)
{
    AST::ExprNode *target = AST::make_member_place(module, self, property.name(), at);

    if (property.has_type() && property.type().is_pointer()) {
        target = &module.nodes.emplace_back<AST::PointerValueNode>(
            target, module.make_virtual_token(property.name(), Token::Type::t_identifier, at));
    }

    auto &write = module.nodes.emplace_back<AST::AssignNode>(target, value, at);

    write.is_initialization = true;

    return write;
}

AST::AssignNode &AST::seat_property_from_parameter(
    AST::Module &module,
    AST::VarDeclNode &self,
    const AST::VarDeclNode &property,
    AST::VarDeclNode *parameter,
    const TokenReference &at
)
{
    auto &param_var = module.nodes.emplace_back<AST::VarNode>(parameter);
    auto &param_read = module.nodes.emplace_back<AST::VarRefNode>(&param_var);

    auto &write = AST::seat_property_from_value(module, self, property, &param_read, at);

    // the parameter was given to this constructor to become part of the value it hands back. a
    // property default does not set this: that expression is a fresh value and is copied
    write.hands_over_value = true;

    return write;
}

void AST::prepend_property_defaults(
    AST::Module &module,
    AST::TypeDeclNode &type,
    AST::FunctionDeclNode &ctor,
    AST::TypeRegistry &registry,
    AST::ScopeNode &declaration_scope
)
{
    // asked of the constructor's own return type, which for a generic is the self-application
    // `Box<T>` and not the template - the same comparison parse_constructor used to make at the
    // call site, and the one AST::is_copy_constructor documents
    if (AST::is_copy_constructor(&ctor, ctor.get_return_type())) {
        return;
    }

    if (ctor.body == nullptr || ctor.body->children.empty()) {
        return;
    }

    // `$this` is the first child by construction - parse_scope seeds it, synthesize_constructor
    // add_vardecl's it, close_constructor_body appends the return behind it
    AST::Node *front = ctor.body->children.front().node();
    assert(front != nullptr && front->get_node_type() == AST::NodeType::n_vardecl
        && "property defaults must seat after $this");
    if (front == nullptr || front->get_node_type() != AST::NodeType::n_vardecl) {
        return;
    }

    auto &this_decl = *static_cast<AST::VarDeclNode *>(front);

    std::vector<AST::NodeReference> seats;

    for (AST::VarDeclNode *prop : type.properties()) {
        if (prop == nullptr || prop->init_expr == nullptr) {
            continue;
        }

        // a fresh context per property, so two defaults cannot collapse onto one clone through the
        // old-to-new map. the substitution is empty: this is the template body, and an instantiation
        // clones the constructor later with T bound
        AST::TypeSubstitution subst;
        AST::CloneContext cc(module.nodes, subst, registry);

        // parse-time: share the closure's FunctionDeclNode rather than cloning it. ClosureExprNode::clone
        // clones the decl so two instances do not share a body still typed with T - here there is no
        // substitution, and a second decl would mangle to the same symbol as the original
        struct RebindClosures : AST::RecursiveVisitor
        {
            AST::CloneContext *cc = nullptr;

            void visit_closure_expr(AST::ClosureExprNode &node) override {
                if (node.decl != nullptr) {
                    cc->map[node.decl] = node.decl;
                }

                AST::RecursiveVisitor::visit_closure_expr(node);
            }
        };

        RebindClosures rebind;
        rebind.cc = &cc;
        prop->init_expr->accept(rebind);

        AST::ExprNode *value = cc.child(prop->init_expr);
        if (value == nullptr) {
            continue;
        }

        // the original was parsed onto a scratch scope in the declaration pass. this is the file
        // root. two constructors sharing one recipe rebind to the same decl, so publish is
        // idempotent on identity
        AST::publish_cloned_closures(*value, &declaration_scope, nullptr, {});

        seats.push_back(AST::make_ref(AST::seat_property_from_value(
            module, this_decl, *prop, value, prop->token_varname)));
    }

    if (seats.empty()) {
        return;
    }

    // after `$this`, never at the front of the body. add_vardecl puts that declaration in children,
    // and for a class its initializer *is* the heap allocation - seating a field before that runs
    // writes through a null handle, the allocation follows, and the property stays zero. a struct
    // hid this: its slot is an alloca hoisted to the entry block, so the write happened to land
    auto &children = ctor.body->children;
    children.insert(children.begin() + 1, seats.begin(), seats.end());
    type.note_defaults_cloned();
}

AST::SynthesizedConstructorKind AST::synthesized_constructor_kind(const AST::TypeDeclNode &type)
{
    bool any_default = false;
    bool all_default = !type.properties().empty();

    for (const AST::VarDeclNode *prop : type.properties()) {
        if (prop != nullptr && prop->init_expr != nullptr) {
            any_default = true;
        } else {
            all_default = false;
        }
    }

    if (any_default && !all_default) {
        return AST::SynthesizedConstructorKind::t_none;
    }

    if (all_default) {
        return AST::SynthesizedConstructorKind::t_zero_arg;
    }

    // **a private property suppresses the field-wise form outright, and that is the whole point of
    // the modifier.** that constructor writes every property from outside the type, so for a type
    // with a hidden one it is a public door into the invariant the modifier exists to keep -
    // `mem::buffer<int32>($stolen, 8)` would be a second owner of one allocation, built with no cast
    // and no `unsafe` anywhere. the zero-arg form does not take those values as arguments, so it
    // returned above without asking
    for (const AST::VarDeclNode *prop : type.properties()) {
        if (prop != nullptr && prop->is_private()) {
            return AST::SynthesizedConstructorKind::t_none;
        }
    }

    return AST::SynthesizedConstructorKind::t_field_wise;
}

void AST::consume_property_defaults(AST::TypeDeclNode &type)
{
    if (!type.defaults_cloned()) {
        return;
    }

    for (AST::VarDeclNode *prop : type.properties()) {
        if (prop != nullptr) {
            prop->init_expr = nullptr;
        }
    }
}
