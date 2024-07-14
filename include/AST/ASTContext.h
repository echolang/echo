#ifndef ASTCONTEXT_H
#define ASTCONTEXT_H

#pragma once

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

        Namespace &current_namespace;

        ScopeNode *scope_ptr = nullptr;

        inline ScopeNode &scope() const {
            assert(scope_ptr);
            return *scope_ptr;
        }
        
        // push & pop the contexts scope
        void push_scope(ScopeNode &scope);
        void pop_scope();

        template <typename T, typename... Args>
            requires NodeTypeProvider<T>
        inline T &emplace_node(Args&&... args) {
            return module.nodes.emplace_back<T>(std::forward<Args>(args)...);
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
    };
};
#endif