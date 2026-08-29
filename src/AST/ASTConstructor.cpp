#include "AST/ASTConstructor.h"

#include "AST/ASTClone.h"
#include "AST/ASTCodeRef.h"
#include "AST/ASTCollector.h"
#include "AST/ASTConstruction.h"
#include "AST/ASTControlFlow.h"
#include "AST/ASTFile.h"
#include "AST/ASTIssue.h"
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
#include "AST/ASTValueType.h"
#include "AST/TypeDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"

#include <fmt/core.h>

#include <cassert>
#include <unordered_set>
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

    AST::VarDeclNode *this_decl = AST::constructor_this(ctor);
    if (this_decl == nullptr) {
        return;
    }

    const std::unordered_set<std::string> derived = AST::derived_fields_of(type);

    std::vector<AST::NodeReference> seats;

    for (AST::VarDeclNode *prop : type.properties()) {
        if (prop == nullptr || prop->init_expr == nullptr) {
            continue;
        }

        // the implicit memberwise ctor seats these from its parameters; cloning the default here
        // would overwrite the argument with the default on every call. the recipe still lives on
        // the parameter, and a closure in it was parsed onto a scratch scope - publish the original
        // onto the file root so codegen emits it
        if (derived.count(prop->name()) != 0) {
            continue;
        }

        if (ctor.is_implicitly_generated && AST::is_implicit_constructor_parameter(*prop)) {
            AST::publish_cloned_closures(*prop->init_expr, &declaration_scope, nullptr, {});
            continue;
        }

        // a fresh context per property, so two defaults cannot collapse onto one clone through the
        // old-to-new map. the substitution is empty: this is the template body, and an instantiation
        // clones the constructor later with T bound
        AST::TypeSubstitution subst;
        AST::CloneContext cc(module.nodes, subst, registry);
        AST::ExprNode *value = AST::clone_sharing_closures(prop->init_expr, cc);
        if (value == nullptr) {
            continue;
        }

        // the original was parsed onto a scratch scope in the declaration pass. this is the file
        // root. two constructors sharing one recipe rebind to the same decl, so publish is
        // idempotent on identity
        AST::publish_cloned_closures(*value, &declaration_scope, nullptr, {});

        seats.push_back(AST::make_ref(AST::seat_property_from_value(
            module, *this_decl, *prop, value, prop->token_varname)));
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

bool AST::is_implicit_constructor_parameter(const AST::VarDeclNode &property)
{
    // statics are not in the layout. private fields are omitted: the implicit ctor is a public
    // door, and a hidden field with no initializer must force a user constructor instead
    return !property.is_static() && !property.is_private();
}

std::vector<AST::VarDeclNode *> AST::implicit_constructor_parameters(const AST::TypeDeclNode &type)
{
    std::vector<AST::VarDeclNode *> params;
    const std::unordered_set<std::string> derived = AST::derived_fields_of(type);

    for (AST::VarDeclNode *prop : type.properties()) {
        if (prop != nullptr && AST::is_implicit_constructor_parameter(*prop)
            && derived.count(prop->name()) == 0) {
            params.push_back(prop);
        }
    }

    return params;
}

const AST::VarDeclNode *AST::uninitialized_private_property(const AST::TypeDeclNode &type)
{
    const std::unordered_set<std::string> derived = AST::derived_fields_of(type);

    for (const AST::VarDeclNode *prop : type.properties()) {
        if (prop != nullptr && prop->is_private() && prop->init_expr == nullptr
            && derived.count(prop->name()) == 0) {
            return prop;
        }
    }

    return nullptr;
}

AST::SynthesizedConstructorKind AST::synthesized_constructor_kind(const AST::TypeDeclNode &type)
{
    // any written constructor deletes memberwise. a copy constructor is a constructor.
    // an uninitialized private is not a reason to skip registration: `init` may still derive
    // it, and the name has to be in the overload set before any other file's body is parsed.
    // finalize_type_construction refuses to build the body if the private is still blank
    if (!type.constructors().empty()) {
        return AST::SynthesizedConstructorKind::t_none;
    }

    return AST::SynthesizedConstructorKind::t_memberwise;
}

AST::VarDeclNode *AST::constructor_this(AST::FunctionDeclNode &ctor)
{
    if (ctor.body == nullptr || ctor.body->children.empty()) {
        return nullptr;
    }

    AST::Node *front = ctor.body->children.front().node();
    if (front == nullptr || front->get_node_type() != AST::NodeType::n_vardecl) {
        return nullptr;
    }

    auto *decl = static_cast<AST::VarDeclNode *>(front);
    return decl->name_full() == "$this" ? decl : nullptr;
}

namespace
{
    class InitPlanter : public AST::RecursiveVisitor
    {
    public:
        InitPlanter(AST::Module &module, AST::FunctionDeclNode *init, AST::VarDeclNode *self) :
            _module(&module), _init(init), _self(self), _at(init->declaration_site_token())
        {}

        void visitFunctionDecl(AST::FunctionDeclNode &) override
        {
            // a nested function is a different frame - its returns are not this constructor's
        }

        void visitScope(AST::ScopeNode &node) override
        {
            // indexed, and the edge is re-read after a splice - RecursiveVisitor::visitScope's rule
            for (size_t i = 0; i < node.children.size(); ) {
                AST::Node *child = node.children[i].node();
                if (child != nullptr && child->get_node_type() == AST::NodeType::n_func_decl) {
                    i++;
                    continue;
                }

                if (child != nullptr && child->get_node_type() == AST::NodeType::n_func_return) {
                    AST::FunctionCallExprNode &call = AST::make_resolved_member_call(
                        *_module, _init, _at, &AST::local_place(*_module, *_self));
                    node.children.insert(
                        node.children.begin() + static_cast<std::ptrdiff_t>(i), AST::make_ref(call));
                    i += 2;
                    continue;
                }

                statement_edge(child);
                i++;
            }
        }

    private:
        AST::Module *_module;
        AST::FunctionDeclNode *_init;
        AST::VarDeclNode *_self;
        TokenReference _at;
    };

    void fill_synthesized_parameters(
        AST::Module &module,
        AST::TypeDeclNode &type,
        AST::FunctionDeclNode &ctor
    )
    {
        ctor.args.clear();

        if (!type.name_token.has_value()) {
            return;
        }

        const TokenReference name_token = type.name_token.value();
        bool copied_a_default = false;

        for (AST::VarDeclNode *prop : AST::implicit_constructor_parameters(type)) {
            auto param_token = module.make_virtual_token(
                "$" + prop->name(), Token::Type::t_varname, name_token);
            auto *param_type = &module.nodes.emplace_back<AST::TypeNode>(prop->type_node()->type);
            auto *param_var = &module.nodes.emplace_back<AST::VarDeclNode>(param_token, param_type);

            param_var->init_expr = prop->init_expr;
            if (prop->init_expr != nullptr) {
                copied_a_default = true;
            }

            ctor.args.push_back(param_var);
        }

        if (copied_a_default) {
            type.note_defaults_cloned();
        }
    }

    void build_synthesized_constructor_body(
        AST::Module &module,
        AST::TypeDeclNode &type,
        AST::FunctionDeclNode &ctor
    )
    {
        if (ctor.body != nullptr || ctor.return_type == nullptr) {
            return;
        }

        const TokenReference name_token = type.name_token.value();
        auto &ctor_body = module.nodes.emplace_back<AST::ScopeNode>();
        ctor.body = &ctor_body;

        auto &this_vardecl = AST::declare_constructor_this(module, *ctor.return_type, name_token);
        ctor.body->add_vardecl(this_vardecl);

        const std::vector<AST::VarDeclNode *> params = AST::implicit_constructor_parameters(type);
        for (size_t i = 0; i < params.size() && i < ctor.args.size(); i++) {
            ctor.body->children.push_back(AST::make_ref(
                AST::seat_property_from_parameter(
                    module,
                    this_vardecl,
                    *params[i],
                    ctor.args[i],
                    params[i]->token_varname)));
        }

        AST::close_constructor_body(module, ctor, this_vardecl);
    }

    void synthesize_constructor(
        AST::Module &module,
        AST::TypeDeclNode &type,
        AST::Collector &collector,
        const AST::ValueType &self_type
    )
    {
        if (AST::synthesized_constructor_kind(type) != AST::SynthesizedConstructorKind::t_memberwise) {
            return;
        }

        if (type.synthesized_constructor() != nullptr) {
            return;
        }

        if (!type.name_token.has_value()) {
            return;
        }

        const TokenReference name_token = type.name_token.value();

        auto &default_ctor = module.nodes.emplace_back<AST::FunctionDeclNode>(name_token);
        default_ctor.member_kind = AST::MemberKind::t_constructor;
        default_ctor.is_implicitly_generated = true;
        default_ctor.ast_namespace = type.ast_namespace;
        default_ctor.type_parameters = type.type_parameters();
        default_ctor.inherited_type_param_count = default_ctor.type_parameters.size();

        type.set_synthesized_constructor(&default_ctor);

        auto &type_node = module.nodes.emplace_back<AST::TypeNode>(self_type);
        default_ctor.return_type = &type_node;

        fill_synthesized_parameters(module, type, default_ctor);
        build_synthesized_constructor_body(module, type, default_ctor);

        collector.functions.register_function(
            collector, AST::CodeRef { &module, name_token.make_slice() }, &default_ctor);
    }
}

void AST::ensure_synthesized_constructor(
    AST::Module &module,
    AST::TypeDeclNode &type,
    AST::Collector &collector,
    const AST::ValueType &self_type
)
{
    if (type.kind() == AST::ComplexTypeKind::t_interface
        || type.kind() == AST::ComplexTypeKind::t_enum) {
        return;
    }

    synthesize_constructor(module, type, collector, self_type);
}

void AST::plant_init_call(AST::Module &module, AST::FunctionDeclNode &ctor, AST::FunctionDeclNode *init)
{
    if (init == nullptr || ctor.body == nullptr) {
        return;
    }

    AST::VarDeclNode *self = AST::constructor_this(ctor);
    if (self == nullptr) {
        return;
    }

    InitPlanter planter(module, init, self);
    ctor.body->accept(planter);
}

void AST::finalize_type_construction(
    AST::Module &module,
    AST::TypeDeclNode &type,
    AST::Collector &collector,
    AST::ScopeNode &declaration_scope,
    const AST::ValueType &self_type
)
{
    if (type.kind() == AST::ComplexTypeKind::t_interface) {
        return;
    }

    if (type.kind() != AST::ComplexTypeKind::t_enum) {
        synthesize_constructor(module, type, collector, self_type);

        if (AST::FunctionDeclNode *synth = type.synthesized_constructor()) {
            if (AST::uninitialized_private_property(type) == nullptr) {
                fill_synthesized_parameters(module, type, *synth);
                synth->body = nullptr;
                build_synthesized_constructor_body(module, type, *synth);
            }
            else {
                // pass 2 built a body so the name existed; `init` did not derive the private,
                // so there is no implicit constructor to emit
                synth->body = nullptr;
            }
        }

        const std::unordered_set<std::string> derived = AST::derived_fields_of(type);

        for (AST::VarDeclNode *prop : type.properties()) {
            if (prop != nullptr && prop->init_expr != nullptr && derived.count(prop->name()) != 0) {
                collector.collect_issue<AST::Issue::DerivedFieldHasDefault>(
                    AST::CodeRef { &module, prop->token_varname.make_slice() },
                    fmt::format(
                        "'{}' is assigned in 'init'; drop the default.",
                        prop->name_full()));
            }
        }

        if (type.constructors().empty()) {
            if (const AST::VarDeclNode *hidden = AST::uninitialized_private_property(type)) {
                collector.collect_issue<AST::Issue::PrivatePropertyNeedsInitializer>(
                    AST::CodeRef { &module, hidden->token_varname.make_slice() },
                    fmt::format(
                        "'{}' is private and has no initializer, so '{}' cannot get an implicit "
                        "constructor. Give the field a default, write a constructor, or assign "
                        "it in 'init'.",
                        hidden->name_full(),
                        type.type_name()));
            }
        }
    }

    for (AST::FunctionDeclNode *ctor : type.constructors()) {
        if (ctor != nullptr) {
            AST::prepend_property_defaults(
                module, type, *ctor, collector.type_registry, declaration_scope);
        }
    }

    if (AST::FunctionDeclNode *synth = type.synthesized_constructor()) {
        AST::prepend_property_defaults(
            module, type, *synth, collector.type_registry, declaration_scope);
    }

    AST::consume_property_defaults(type);

    if (AST::FunctionDeclNode *init = type.complex_type().type_init()) {
        for (AST::FunctionDeclNode *ctor : type.constructors()) {
            if (ctor != nullptr) {
                AST::plant_init_call(module, *ctor, init);
            }
        }

        if (AST::FunctionDeclNode *synth = type.synthesized_constructor()) {
            AST::plant_init_call(module, *synth, init);
        }
    }

    if (AST::FunctionDeclNode *synth = type.synthesized_constructor()) {
        if (synth->body != nullptr) {
            declaration_scope.add_funcdecl(*synth);
        }
    }
}

void AST::finalize_module_construction(AST::Module &module, AST::Collector &collector)
{
    // arena sweep, not a tree walk: a TypeDeclNode is in the file root's children, but it is
    // also the object the declaration pass created, and of_type is how every other post-parse
    // sweep finds types. a tree walk from the file root missed them in practice, and a missed
    // type keeps the wide pass-2 parameter list
    module.construction_finalized = true;

    for (AST::TypeDeclNode *type : module.nodes.of_type<AST::TypeDeclNode>()) {
        if (type == nullptr || !type->name_token.has_value()) {
            continue;
        }

        AST::File *file = type->name_token->file();
        AST::ScopeNode *root = file != nullptr ? file->root : nullptr;

        if (root == nullptr) {
            for (AST::File &entry : module.files()) {
                if (entry.root != nullptr) {
                    root = entry.root;
                    break;
                }
            }
        }

        if (root == nullptr) {
            continue;
        }

        AST::finalize_type_construction(
            module, *type, collector, *root, type->value_type());
    }
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
