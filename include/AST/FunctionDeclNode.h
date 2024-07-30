#ifndef FUNCTIONNODE_H
#define FUNCTIONNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"

#include "ScopeNode.h"
#include "VarDeclNode.h"
#include <optional>

namespace AST 
{
    class Namespace;

    class FunctionDeclNode : public Node
    {
    public:
        static constexpr NodeType node_type = NodeType::n_func_decl;
            
        std::optional<TokenReference> name_token;
        std::vector<VarDeclNode*> args;
        TypeNode *return_type = nullptr;
        Namespace *ast_namespace = nullptr;
        ScopeNode *body = nullptr;

        FunctionDeclNode() {};
        FunctionDeclNode(TokenReference name_token) :
            name_token(name_token)
        {};

        ~FunctionDeclNode() {};

        inline bool is_anonymous() const {
            return !name_token.has_value();
        }

        const std::string func_name() const {
            if (name_token.has_value()) {
                return name_token.value().value();
            }

            return "[anonymous]";
        }

        // returns the decorated function name as it would appear in the symbol table
        // this is the name that is used to uniquely identify the function aka the mangled name
        const std::string decorated_func_name() const;
        
        const std::string namespaced_func_name() const;

        const std::string get_return_type_description() {
            if (return_type) {
                return return_type->node_description();
            }

            return "[unknown]";
        }

        const ValueType get_return_type() {
            if (return_type) {
                return return_type->type;
            }

            return ValueType::void_type();
        }

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visitFunctionDecl(*this);
        }

    private:

    };
};

#endif