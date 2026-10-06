#ifndef LSPCOMPLETIONLOOKUP_H
#define LSPCOMPLETIONLOOKUP_H

#pragma once

#include "AST/ASTDeclarationOrigin.h"
#include "AST/ASTValueType.h"
#include "AST/ASTVisibility.h"
#include "Compiler/Lsp/LspCompletion.h"

#include <optional>
#include <string>
#include <vector>

namespace AST
{
    class Bundle;
    class ComplexType;
    class FunctionDeclNode;
    class Namespace;
    class Node;
    class Symbol;
    class TypeDeclNode;
};

namespace Compiler
{
    namespace Lsp
    {
        const AST::ComplexType *complex_of(const AST::ValueType &type);
        std::vector<AST::FunctionDeclNode *> methods_of(const AST::ComplexType &type);

        // snapshot knowledge at the cursor: which namespace, which type a chain names, who can
        // see a declaration. completion asks; this answers. a miss is silence: the statement
        // being typed is in no snapshot
        class CompletionLookup
        {
        public:

            explicit CompletionLookup(const CompletionRequest &request);

            const CompletionContext &context() const {
                return _context;
            }

            AST::Bundle *bundle() const {
                return _bundle;
            }

            const AST::Namespace *current_namespace() const {
                return _namespace;
            }

            const AST::ComplexType *enclosing_type() const {
                return _enclosing;
            }

            const AST::DeclarationOrigin &origin() const {
                return _origin;
            }

            bool visible(
                AST::Visibility visibility,
                const AST::DeclarationOrigin &origin,
                const AST::ComplexType *owner
            ) const;
            bool visible(const AST::FunctionDeclNode &fn) const;

            AST::TypeDeclNode *type_of_symbol(const AST::Symbol *symbol) const;
            const AST::Namespace *resolve_namespace_path(const std::vector<std::string> &path) const;
            AST::TypeDeclNode *resolve_type_path(const std::vector<std::string> &path) const;
            AST::TypeDeclNode *resolve_type_text(std::string text) const;
            const AST::FunctionDeclNode *first_overload(
                const std::string &name,
                const AST::Namespace &ns
            ) const;
            const AST::FunctionDeclNode *enclosing_function() const;

            std::optional<AST::ValueType> type_of_lexical(const LexicalVariable &variable);
            std::optional<AST::ValueType> type_of_chain(
                const std::vector<ReceiverSegment> &chain,
                bool optional = false
            );

        private:

            const CompletionRequest &_request;
            CompletionContext _context;
            AST::Bundle *_bundle = nullptr;
            const AST::Namespace *_namespace = nullptr;
            AST::DeclarationOrigin _origin;
            const AST::ComplexType *_enclosing = nullptr;
            int _depth = 0;

            void settle_scope();
            const AST::Namespace *namespace_at_cursor();
            AST::Node *node_at(size_t offset) const;
            AST::TypeDeclNode *type_decl_named_at(size_t offset, const std::string &name) const;
            const AST::Namespace *child_namespace(
                const AST::Namespace &parent,
                const std::vector<std::string> &segments
            ) const;
            const AST::ImportBinding *import_named(const std::string &name) const;
            std::optional<AST::ValueType> type_of_variable(const std::string &name);
            std::optional<AST::ValueType> type_of_root(const ReceiverSegment &root);
            std::optional<AST::ValueType> type_of_index(const AST::ValueType &current) const;
        };
    };
};

#endif
