#ifndef VARNODE_H
#define VARNODE_H

#pragma once

#include "ASTNode.h"
#include "VarDeclNode.h"

namespace AST 
{   
    /**
     * A var is basically just a reference to a declaration and a token
     * But do not confuse this with the "VarRefNode" which can either contain:
     *  - a "VarNode" meaning a reference to a whole variable
     *  - a "VarMemberNode" meaning a reference to a member of a variable
     *  - a "VarRefNode" meaning an intend to reference a value of a variable
     *    which can target both a "VarNode" or a "VarMemberNode"
     */
    class VarNode : public Node
    {
    public:
        static constexpr NodeType node_type = NodeType::n_var;

        VarNode(VarDeclNode *decl) :
            _decl(decl)
        {
            assert(decl != nullptr && "You must provide a var declaration for a var node");
        };

        VarNode(VarDeclNode *decl, TokenReference token_varname) :
            _token_varname(token_varname), _decl(decl)
        {
            assert(decl != nullptr && "You must provide a var declaration for a var node");
        };
        ~VarNode() {};
        
        const std::string node_description() override {
            return "var(" + _decl->name_full() + ")";
        }

        void accept(Visitor& visitor) override {
            visitor.visitVar(*this);
        }

        Node *clone(CloneContext &cc) const override;

        inline VarDeclNode &decl() const {
            assert(_decl);
            return *_decl;
        }

        inline VarDeclNode *try_decl() const {
            return _decl;
        }
    private:
        std::optional<TokenReference> _token_varname;
        VarDeclNode *_decl;
    };
};

#endif