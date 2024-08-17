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
        static constexpr NodeType node_type = NodeType::n_struct_decl;
        
        Namespace *ast_namespace = nullptr;
        
        std::optional<TokenReference> name_token;

        StructDeclNode(TokenReference name_token) :
            name_token(name_token)
        {
            _complex_type = ComplexType(name_token.value());
            _type = ValueType::make_struct(&_complex_type);
        };

        ~StructDeclNode() {};

        const std::string struct_name() const;

        const std::string namespaced_struct_name() const;

        const std::string node_description() override;

        ValueType value_type() const {
            return _type;
        }

        // declares this struct's generic type parameters (e.g. the T, U in `struct Foo<T, U>`).
        // replaces any previously set list so the symbol pass and full parse stay idempotent.
        void set_type_parameters(const std::vector<std::string> &names) {
            _complex_type.type_parameters.clear();
            for (const auto &name : names) {
                _complex_type.type_parameters.push_back(ComplexType::TypeParam{name});
            }
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