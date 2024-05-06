#ifndef ASTMODULE_H
#define ASTMODULE_H

#pragma once

#include "../Token.h"
#include "ASTNode.h"
#include "ASTFile.h"
#include "ASTNodeReference.h"
#include "ScopeNode.h"

#include <filesystem>
#include <unordered_map>
#include <vector>
#include <memory>

class Lexer;

namespace AST
{   
    typedef size_t module_handle_t;

    class Module
    {
    public:
        TokenCollection tokens = TokenCollection();
        NodeCollection nodes = NodeCollection();

        const std::string name;
        const module_handle_t handle;

        Module(const std::string &name, module_handle_t handle) : 
            name(name), handle(handle) 
        {}
        ~Module() {}

        std::string debug_description() const;

        File &add_file(const std::filesystem::path &path);
        TokenizedFile &tokenize(Lexer &lexer, const File &file);

        bool is_owner_of(const TokenReference &tokenref) const {
            return tokenref.belongs_to(tokens);
        }

    private:

        std::vector<std::unique_ptr<File>> _files;
        std::vector<TokenizedFile> _tokenized_files;

    };

    struct ModuleCollection
    {
        ModuleCollection() {}
        ~ModuleCollection() {}

        module_handle_t add_module(const std::string &name);

        Module &get_module(module_handle_t handle) {
            return *_modules[handle].get();
        }

        inline bool is_valid_handle(module_handle_t handle) {
            return handle < _modules.size();
        }

        Module *find_module_ptr(const std::string &name);

        Module &find_module(const std::string &name);

        bool has_module(const std::string &name);

        private: 
            std::vector<std::unique_ptr<Module>> _modules;
            std::unordered_map<std::string, module_handle_t> _module_map;
    };

};
#endif