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
    class FunctionDeclNode : public Node
    {
    public:
        static constexpr NodeType node_type = NodeType::n_func_decl;

        std::optional<TokenReference> name_token;
        std::vector<VarDeclNode*> args;
        TypeNode *return_type = nullptr;
        ScopeNode* body = nullptr;

        FunctionDeclNode() {};
        FunctionDeclNode(TokenReference name_token) :
            name_token(name_token)
        {};

        ~FunctionDeclNode() {};

        const std::string func_name() {
            if (name_token.has_value()) {
                return name_token.value().value();
            }

            return "[anonymous]";
        }

        const std::string get_return_type_description() {
            if (return_type) {
                return return_type->node_description();
            }

            return "[unknown]";
        }

        const std::string node_description() override {
            return "function " + func_name() + " -> " + get_return_type_description() + "\n" + body->node_description();
        }

        void accept(Visitor &visitor) override {
            visitor.visitFunctionDecl(*this);
        }

    private:

    };
};

#endif