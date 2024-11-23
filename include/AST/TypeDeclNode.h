#ifndef TYPEDECLNODE_H
#define TYPEDECLNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTNamespace.h"
#include "Token.h"
#include "ASTValueType.h"

namespace AST 
{
    class TypeDeclNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_type_decl);
        
        Namespace *ast_namespace = nullptr;
        
        std::optional<TokenReference> name_token;

        TypeDeclNode(TokenReference name_token, ComplexTypeKind kind = ComplexTypeKind::t_struct) :
            name_token(name_token)
        {
            _complex_type = ComplexType(name_token.value());
            _complex_type.kind = kind;
        };

        ~TypeDeclNode() {};

        // assigns the declaring namespace. the complex type has to learn it too, so its mangled
        // name and description can be fully qualified - always set it through here
        void set_namespace(Namespace *ns) {
            ast_namespace = ns;
            _complex_type.ast_namespace = ns;
        }

        // the token this struct is declared at. every parse pass walks identical token indices, so the
        // position a declaration is *written* at identifies it - which the name cannot, because the
        // name is exactly what two declarations of the same struct share. the same identity
        // FunctionRegistry keys a function's declaration site on
        const TokenReference &declaration_site_token() const {
            return name_token.value();
        }

        bool is_declared_at(const TokenReference &token) const {
            return name_token.has_value()
                && token.belongs_to(name_token->get_collection_ref())
                && token.get_handle() == name_token->get_handle();
        }

        const std::string type_name() const;

        const std::string namespaced_type_name() const;

        const std::string node_description() override;

        // computed rather than stored. a stored ValueType would have to point at the embedded
        // _complex_type, and CloneContext::shallow copy-constructs this node - so the copy's stored
        // type would point at the *original's* layout until something rebuilt it by hand, which is
        // the hazard this avoids. there is nothing to keep in sync if there is nothing stored
        ValueType value_type() const {
            return ValueType::make_complex(const_cast<ComplexType *>(&_complex_type));
        }

        ComplexTypeKind kind() const {
            return _complex_type.kind;
        }

        bool is_class() const {
            return _complex_type.is_class_kind();
        }

        // the embedded complex type, which owns this struct's generic type parameters (the T, U
        // in `struct Foo<T, U>`). exposed so the parser's declaring step can install them
        ComplexType &complex_type() {
            return _complex_type;
        }

        const std::vector<TypeParamDecl *> &type_parameters() const {
            return _complex_type.type_parameters;
        }

        bool is_generic() const {
            return _complex_type.is_generic();
        }

        void accept(Visitor& visitor) override {
            visitor.visit_type_decl(*this);
        }

        Node *clone(CloneContext &cc) const override;

        void add_property(VarDeclNode *property);

        const std::vector<VarDeclNode *> &properties() const {
            return _properties;
        }

        // the member functions declared in this struct's body. unlike properties, which this node
        // keeps as VarDeclNodes alongside the ComplexType's flattened layout, a method is the same
        // pointer in both places - so it is stored once, on the type. the type is what a receiver
        // names, and an instantiation has a ComplexType but no TypeDeclNode of its own.
        // FunctionRegistry::register_member_function is what appends
        const std::vector<FunctionDeclNode *> &methods() const {
            return _complex_type.methods();
        }

        // the constructors the user wrote, in declaration order. a constructor is a free function
        // named after the struct rather than a member, so it is not in _complex_type - but the
        // struct still has to know its own, because that is what the field-wise constructor's
        // suppression rule compares against. never cleared between parse passes: the passes
        // reconcile on the declaration site, so a second pass finds the same node
        void add_constructor(FunctionDeclNode *constructor) {
            _constructors.push_back(constructor);
        }

        const std::vector<FunctionDeclNode *> &constructors() const {
            return _constructors;
        }

        // the synthesized field-wise constructor, or null until it is built. kept apart from the
        // user's own so the suppression rule cannot compare it against itself
        void set_field_wise_constructor(FunctionDeclNode *constructor) {
            _field_wise_constructor = constructor;
        }

        FunctionDeclNode *field_wise_constructor() const {
            return _field_wise_constructor;
        }

        // whether some pass has already taken this struct's properties. the body is walked in both
        // parse passes with the same code - so the two cannot disagree about where a property ends,
        // and a generic application in a property type is re-interned once every template layout is
        // complete - but only the first walk may *keep* what it parsed, or the layout is doubled.
        //
        // a flag on the node rather than a check on the pass, so that a struct the declaration pass
        // never reached (error recovery skipped past it) is still collected by the body pass instead
        // of ending up with an empty layout
        bool members_collected() const {
            return _members_collected;
        }

        void mark_members_collected() {
            _members_collected = true;
        }

    private:
        ComplexType _complex_type;
        std::vector<VarDeclNode *> _properties;
        std::vector<FunctionDeclNode *> _constructors;
        FunctionDeclNode *_field_wise_constructor = nullptr;
        bool _members_collected = false;
    };
};

#endif