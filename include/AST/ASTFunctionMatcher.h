#ifndef ASTFUNCTIONMATCHER_H
#define ASTFUNCTIONMATCHER_H

#pragma once

#include <unordered_map>
#include <vector>

namespace AST
{
    class FunctionDeclNode;
    class FunctionCallExprNode;
    class Namespace;

    class FunctionRegistry
    {
    public:
        typedef size_t function_handle_t;
        typedef size_t namespaced_handle_t;

        struct Namespaced {
            AST::Namespace *ns;
            std::unordered_map<std::string, std::vector<function_handle_t>> functions;
        };

        FunctionRegistry() {
            // first function is reserved for "no function"
            _functions.push_back(nullptr);
        }
        ~FunctionRegistry() {}

        function_handle_t register_function(FunctionDeclNode *func_decl);

        inline std::vector<FunctionDeclNode *> &get_all() {
            return _functions;
        }

        inline const std::vector<FunctionDeclNode *> &get_all() const {
            return _functions;
        }

        inline const FunctionDeclNode *get_function(function_handle_t handle) const {
            return _functions[handle];
        }
        
        function_handle_t match_function(const FunctionCallExprNode *call_expr, AST::Namespace *ns = nullptr) const;

        std::vector<function_handle_t> find_function_by_name(const std::string &name, const AST::Namespace *ns = nullptr) const;

        std::vector<function_handle_t> find_function_by_signature(const std::string &name, const AST::Namespace *ns = nullptr) const;



    private:

        std::vector<FunctionDeclNode *> _functions;
        
        std::vector<Namespaced> _namespaced_fncname;
        std::vector<Namespaced> _namespaced_fncsig;

        std::unordered_map<AST::Namespace *, namespaced_handle_t> _namespace_map;
    };
}

#endif