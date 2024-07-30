#ifndef SYMBOLTABLE_H
#define SYMBOLTABLE_H

#pragma once

#include "AST/FunctionDeclNode.h"
#include "AST/ASTMangler.h"

#include <llvm/IR/Function.h>

namespace Compiler::LLVM
{   
    typedef size_t function_id_t;

    struct Function 
    {
        const AST::FunctionDeclNode *ast_funcdecl;
        llvm::Function *llvm_func;
    };

    class FunctionTable
    {
        std::vector<Function> _functions;
        std::vector<std::string> _function_names;
        std::unordered_map<std::string, function_id_t> _function_name_map;
        std::unordered_map<const AST::FunctionDeclNode*, function_id_t> _function_ast_map;

    public:

        FunctionTable() {
            // always insert a dummy function at index 0
            // because 0 is reserved for "no function"
            _functions.push_back({ nullptr, nullptr });
            _function_names.push_back("");
        };

        function_id_t push_function(std::string mangled_name, const AST::FunctionDeclNode *funcdecl, llvm::Function *func) {
            _functions.push_back({ funcdecl, func });
            _function_names.push_back(mangled_name);

            auto id = _functions.size() - 1;
            _function_name_map[mangled_name] = id;
            _function_ast_map[funcdecl] = id;

            return id;
        }

        Function& get_function(function_id_t id) {
            return _functions[id];
        }

        const Function& get_function(function_id_t id) const {
            return _functions[id];
        }

        llvm::Function* get_llvm_function(function_id_t id) {
            return _functions[id].llvm_func;
        }

        const llvm::Function* get_llvm_function(function_id_t id) const {
            return _functions[id].llvm_func;
        }

        function_id_t get_function_id(const std::string &name) const {
            auto it = _function_name_map.find(name);
            if (it != _function_name_map.end()) {
                return it->second;
            }
            return 0;
        }

        function_id_t get_function_id(const AST::FunctionDeclNode *funcdecl) const {
            auto it = _function_ast_map.find(funcdecl);
            if (it != _function_ast_map.end()) {
                return it->second;
            }

            return 0;
        }

        function_id_t get_function_id_by_name(const std::string &name) const {
            auto it = _function_name_map.find(name);
            if (it != _function_name_map.end()) {
                return it->second;
            }

            return 0;
        }
    };
}


#endif