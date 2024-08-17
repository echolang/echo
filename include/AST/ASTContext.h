#ifndef ASTCONTEXT_H
#define ASTCONTEXT_H

#pragma once

#include <algorithm>
#include "ASTModule.h"
#include "ASTFile.h"
#include "ASTCodeRef.h"
#include "ASTNamespace.h"

namespace AST
{  
    struct Context
    {
        Module &module;

        const TokenizedFile &file;

        Namespace *current_namespace;

        ScopeNode *scope_ptr = nullptr;
        
        // Type parameters for the current function being parsed (for generics)
        std::vector<std::string> current_type_parameters;

        inline ScopeNode &scope() const {
            assert(scope_ptr);
            return *scope_ptr;
        }
        
        // push & pop the contexts scope
        void push_scope(ScopeNode &scope);
        void pop_scope();
        
        // Type parameter management for generic functions
        void set_type_parameters(const std::vector<std::string>& params) {
            current_type_parameters = params;
        }
        
        void clear_type_parameters() {
            current_type_parameters.clear();
        }
        
        bool is_type_parameter(const std::string& name) const {
            return std::find(current_type_parameters.begin(), current_type_parameters.end(), name) != current_type_parameters.end();
        }

        template <typename T, typename... Args>
            requires NodeTypeProvider<T>
        inline T &emplace_node(Args&&... args) {
            return module.nodes.emplace_back<T>(std::forward<Args>(args)...);
        }

        // same as emplace_node but will return a pointer to the node instead of a reference
        template <typename T, typename... Args>
            requires NodeTypeProvider<T>
        inline T *emplace_nodep(Args&&... args) {
            return &module.nodes.emplace_back<T>(std::forward<Args>(args)...);
        }

        CodeRef code_ref() const {
            return CodeRef { &module, file.file, file.token_slice };
        }

        CodeRef code_ref(const TokenSlice &slice) const {
            return CodeRef { &module, file.file, slice };
        }

        CodeRef code_ref(const TokenReference &tokenref) const {
            if (!module.is_owner_of(tokenref)) {
                throw std::runtime_error("TokenReference does not belong to this module");
            }

            return CodeRef { &module, file.file, tokenref.make_slice() };
        }

        TokenReference make_virtual_token(const std::string &value, Token::Type type, size_t line, size_t char_offset) {
            auto ti = module.tokens.push(value, type, line, char_offset);
            return module.tokens[ti];
        }

        /**
         * Creates a virtual token with the same position as the reference token
         */
        TokenReference make_virtual_token(const std::string &value, Token::Type type, const TokenReference &ref) {
            return make_virtual_token(value, type, ref.line(), ref.char_offset());
        }
    };
};
#endif