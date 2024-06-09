#ifndef VARDECLNODE_H
#define VARDECLNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "../Lexer.h"
#include "TypeNode.h"

namespace AST 
{
    class ExprNode;

    class VarDeclNode : public Node
    {
        // node that declared the type of this variable
        TypeNode *_type_node;

    public:
    
        TokenReference token_varname;


        // the expression that initializes this variable
        ExprNode *init_expr = nullptr;

        // the name of variable without the $ prefix
        std::string symbol_name;

        VarDeclNode(TokenReference token_varname, TypeNode *type) : 
            _type_node(type), token_varname(token_varname)
        {
            symbol_name = token_varname.value().substr(1);
        };

        ~VarDeclNode() {};

        static constexpr NodeType node_type = NodeType::n_vardecl;

        const std::string &name_full() const {
            return token_varname.value();
        }

        const std::string &name() const {
            return symbol_name;
        }

        inline TypeNode *type_node() const {
            assert(_type_node != nullptr && "type node is null");
            return _type_node;
        }

        // same as type_node but can won't assert if the type is not set
        inline TypeNode *optional_type_node() const {
            return _type_node;
        }

        inline bool has_type() const {
            return _type_node != nullptr;
        }

        void set_type_node(TypeNode *type) {
            _type_node = type;
        }
        
        const std::string node_description() override;

        void accept(Visitor& visitor) override {
            visitor.visitVarDecl(*this);
        }

    private:

    };
};

#endif