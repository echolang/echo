#ifndef VARDECLNODE_H
#define VARDECLNODE_H

#pragma once

#include "ASTNode.h"
#include "ASTValueType.h"
#include "Lexer.h"
#include "TypeNode.h"

namespace AST 
{
    class ExprNode;

    class VarDeclNode : public Node
    {
        // node that declared the type of this variable
        TypeNode *_type_node;

        VarDeclNode *points_to = nullptr;

    public:
    
        TokenReference token_varname;


        // the expression that initializes this variable
        ExprNode *init_expr = nullptr;

        // the name of variable without the $ prefix
        std::string symbol_name;

        // written `mv Buffer $items`: this *parameter* takes ownership of its argument, so a caller
        // handing it a place has to say `mv` at the call site. always false for a local, which owns
        // whatever it is initialised with either way
        //
        // deliberately here and not on the ValueType. that struct is the interning identity for
        // TypeRegistry and for the monomorphizer's instance cache, so a flag on it would fork every
        // owning type in two and make `Buffer` and `mv Buffer` distinct types. the consequence is
        // that `consume(mv Buffer)` and `consume(Buffer)` mangle identically and collide as a
        // DuplicateFunctionSignature - which is correct: `mv` is a contract about the argument, not
        // a distinction a call can be resolved on
        bool takes_ownership = false;

        // written `guard T $x = <nullable> else {...}`: this declaration's initializer is one level more
        // *nullable* than the declaration is, because the statement around it tests the value and only
        // binds on the path where it was there
        //
        // so the ordinary "does the initializer fit the declared type" rule does not apply, and the one
        // check that has to know is AST::TypeChecker's - which would otherwise report exactly the
        // conversion the guard exists to perform. everything else about the declaration is ordinary: it is
        // a local in the enclosing scope, the ownership pass makes it an owner, and it is dropped at the
        // scope's end like any other. that is the whole reason this is one bit rather than a second kind
        // of declaration
        //
        // here rather than on the ValueType for takes_ownership's reason above: it is a fact about *this
        // declaration*, not a distinction two types could be told apart by
        bool binds_unwrapped = false;

        VarDeclNode(TokenReference token_varname, TypeNode *type) :
            _type_node(type), token_varname(token_varname)
        {
            symbol_name = token_varname.value().substr(1);
        };

        ~VarDeclNode() {};

        ECO_AST_NODE_TYPE(n_vardecl);

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

        inline ValueType type() const {
            return _type_node->type;
        }

        inline bool has_type() const {
            return _type_node != nullptr;
        }

        void set_type_node(TypeNode *type) {
            _type_node = type;
        }
        
        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visitVarDecl(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:

    };
};

#endif