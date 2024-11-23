#ifndef VARNODE_H
#define VARNODE_H

#pragma once

#include "ASTNode.h"
#include "VarDeclNode.h"

namespace AST 
{   
    /**
     * A var is basically just a reference to a declaration and a token
     * But do not confuse this with the "VarRefNode", which wraps a "VarNode"
     * to express an intent to reference the value of a variable
     */
    class VarNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_var);

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

        void accept(Visitor &visitor) override {
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

        // where *this* mention of the variable is written, falling back to the declaration for a
        // reference the parser synthesized rather than read (a constructor's `$this`, a drop's
        // receiver). the single answer to "where do I point a diagnostic about this use" - without
        // it, "'$a' has been moved out of" lands on the declaration and says nothing about which
        // read was the offending one
        inline const TokenReference &use_token() const {
            return _token_varname.has_value() ? _token_varname.value() : _decl->token_varname;
        }
    private:
        std::optional<TokenReference> _token_varname;
        VarDeclNode *_decl;
    };
};

#endif