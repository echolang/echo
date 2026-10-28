#ifndef TYPEALIASDECLNODE_H
#define TYPEALIASDECLNODE_H

#pragma once

#include "AST/ASTDeclarationOrigin.h"
#include "AST/ASTNode.h"
#include "AST/ASTVisibility.h"
#include "Lexer.h"

#include <cstddef>
#include <vector>

namespace AST
{
    class Namespace;
    class TypeNode;
    class TypeParamDecl;

    // how far the alias step between the first two parse passes got with this alias's target
    enum class TypeAliasResolution
    {
        t_unresolved,
        t_resolved,
        t_cyclic,
    };

    // `type CommentQuery = carbon::Query<int64, Comment, CommentRepository>;` at file or namespace scope.
    // transparent: a use of the name is its target, substituted through its own type parameters
    class TypeAliasDeclNode : public Node
    {
    public:
        ECO_AST_NODE_TYPE(n_type_alias);

        TokenReference token_name;

        Namespace *ast_namespace = nullptr;
        DeclarationOrigin declared_in;
        Visibility visibility = Visibility::t_module;
        std::string doc_comment;

        std::vector<TypeParamDecl *> type_parameters;

        // the written target, parsed once every type name exists and again by the body pass
        TypeNode *target = nullptr;
        TypeAliasResolution resolution = TypeAliasResolution::t_unresolved;

        // where the declaration starts in the module's tokens, at the `type` word
        size_t decl_index = 0;
        size_t decl_end = 0;

        TypeAliasDeclNode(TokenReference token_name) :
            token_name(token_name)
        {};

        ~TypeAliasDeclNode() {};

        const std::string &name() const {
            return token_name.value();
        }

        // the target once the alias step settled it; null while pending, cyclic or failed
        const TypeNode *resolved_target() const {
            return resolution == TypeAliasResolution::t_resolved ? target : nullptr;
        }

        bool is_generic() const {
            return !type_parameters.empty();
        }

        bool is_declared_at(const TokenReference &token) const {
            return token.belongs_to(token_name.get_collection_ref())
                && token.get_handle() == token_name.get_handle();
        }

        const std::string node_description() override;

        void accept(Visitor &visitor) override {
            visitor.visit_type_alias(*this);
        }

        Node *clone(CloneContext &cc) const override;
    };
};

#endif
