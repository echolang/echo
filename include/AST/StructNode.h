#ifndef STRUCTNODE_H
#define STRUCTNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTNamespace.h"
#include "Token.h"
#include "ASTValueType.h"

namespace AST 
{
    class StructDeclNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_struct_decl);
        
        Namespace *ast_namespace = nullptr;
        
        std::optional<TokenReference> name_token;

        StructDeclNode(TokenReference name_token) :
            name_token(name_token)
        {
            _complex_type = ComplexType(name_token.value());
            _type = ValueType::make_struct(&_complex_type);
        };

        ~StructDeclNode() {};

        // assigns the declaring namespace. the complex type has to learn it too, so its mangled
        // name and description can be fully qualified - always set it through here
        void set_namespace(Namespace *ns) {
            ast_namespace = ns;
            _complex_type.ast_namespace = ns;
        }

        const std::string struct_name() const;

        const std::string namespaced_struct_name() const;

        const std::string node_description() override;

        ValueType value_type() const {
            return _type;
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
            visitor.visitStructDecl(*this);
        }

        Node *clone(CloneContext &cc) const override;

        void add_property(VarDeclNode *property);

        const std::vector<VarDeclNode *> &properties() const {
            return _properties;
        }

    private:
        ValueType _type;
        ComplexType _complex_type;
        std::vector<VarDeclNode *> _properties;
    };
};

#endif