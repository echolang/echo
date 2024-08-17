#ifndef VARMEMBERNODE_H
#define VARMEMBERNODE_H

#pragma once

#include <optional>
#include "Token.h"
#include "ASTNode.h"
#include "ASTValueType.h"

namespace AST 
{   
    class VarRefNode;

    class VarMemberNode : public Node
    {
    public:
        static constexpr NodeType node_type = NodeType::n_varmember;

        VarMemberNode(VarRefNode *ref) :
            _ref(ref)
        {
            assert(ref != nullptr && "You must provide a var reference for a var member node");
        };
        ~VarMemberNode() {};
        
        const std::string node_description() override;

        const StructDeclNode *struct_decl() const;
        const ComplexType::Property &property() const;

        inline VarRefNode& get_ref() const {
            assert(_ref != nullptr);
            return *_ref;
        }

        void accept(Visitor& visitor) override {
            visitor.visitVarMember(*this);
        }

        Node *clone(CloneContext &cc) const override;

    private:
        std::optional<TokenReference> _token_member;
        VarRefNode *_ref;
        size_t _member_index = 0;
    };
};

#endif