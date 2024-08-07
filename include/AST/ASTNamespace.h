#ifndef ASTNAMESPACE_H
#define ASTNAMESPACE_H

#pragma once

#include <string>
#include <unordered_map>
#include <memory>

#include "ASTSymbol.h"

#define ECO_NAMESPACE_SEPARATOR "::"

namespace AST
{
    class Namespace
    {
        friend class NamespaceManager;
        
    public:

        Namespace(const std::string &name) : _name(name) {};
        ~Namespace() {};

        // disallow copy and move
        Namespace(const Namespace &) = delete;
        Namespace(Namespace &&) = delete;

        std::string name() const { 
            return _name; 
        }

        const Namespace *parent() const { 
            return _parent; 
        }

        void push_symbol(std::unique_ptr<Symbol> symbol);

        std::string debug_dump_symbols() const;

    private:
        Namespace *_parent = nullptr;
        std::string _name;
        std::unordered_map<std::string, std::unique_ptr<Namespace>> _children;
        std::unordered_map<std::string, std::unique_ptr<Symbol>> _symbols;
    };

    class NamespaceManager
    {
    public:
        NamespaceManager() : _root("") {};
        ~NamespaceManager() {};

        // returns the namespace for the given name, creating it if it doesn't exist
        Namespace &retrieve(const std::string &name);
        Namespace &retrieve(const std::vector<std::string> &parts);

        // returns the namespace for the given name, or nullptr if it doesn't exist
        const Namespace *get(const std::string &name) const;
        const Namespace *get(const std::vector<std::string> &parts) const;

        bool exists(const std::string &name) const;
        bool exists(const std::vector<std::string> &parts) const;

        Symbol *find_symbol(const std::string &fullname) const;
        Symbol *find_symbol(const std::string &symbol_name, const std::string &ns) const;
        Symbol *find_symbol(const std::string &symbol_name, const Namespace &ns) const;

        Namespace &root() { return _root; }

    private:
        Namespace _root;
    };
};

#endif