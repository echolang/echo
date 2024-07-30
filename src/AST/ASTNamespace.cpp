#include "AST/ASTNamespace.h"
#include "AST/ASTSymbol.h"

#include "Debugging.h"

#include <vector>

std::vector<std::string> split_namespace(const std::string &str) {
    std::string delimiter = ECO_NAMESPACE_SEPARATOR;
    std::vector<std::string> parts;
    size_t start = 0, end = 0;

    while ((end = str.find(delimiter, start)) != std::string::npos) {
        parts.push_back(str.substr(start, end - start));
        start = end + delimiter.length();
    }

    parts.push_back(str.substr(start)); // add the last part

    return parts;
}

AST::Namespace &AST::NamespaceManager::retrieve(const std::string &name)
{
    // explode the name by the namespace separator
    std::vector<std::string> parts = split_namespace(name);

    return retrieve(parts);
}

AST::Namespace &AST::NamespaceManager::retrieve(const std::vector<std::string> &parts)
{
    // start from the root
    auto current = &_root;

    // iterate over the parts and find the namespace
    for (const auto &part : parts)
    {
        if (current->_children.find(part) == current->_children.end()) {
            current->_children[part] = std::make_unique<Namespace>(part);
            current->_children[part]->_parent = current;
        }

        current = current->_children[part].get();
    }

    return *current;
}

const AST::Namespace *AST::NamespaceManager::get(const std::string &name) const
{
    // explode the name by the namespace separator
    std::vector<std::string> parts = split_namespace(name);

    return get(parts);
}

const AST::Namespace *AST::NamespaceManager::get(const std::vector<std::string> &parts) const
{
    // start from the root
    auto current = &_root;

    // iterate over the parts and find the namespace
    for (const auto &part : parts)
    {
        if (current->_children.find(part) == current->_children.end()) {
            return nullptr;
        }

        current = current->_children.at(part).get();
    }

    return current;
}

bool AST::NamespaceManager::exists(const std::vector<std::string> &parts) const 
{
    return get(parts) != nullptr;
}

bool AST::NamespaceManager::exists(const std::string &name) const
{
    return get(name) != nullptr;
}

AST::Symbol *AST::NamespaceManager::find_symbol(const std::string &fullname) const
{
    // split the string by the last namespace separator 
    // this is how we differentiate between the symbol name and the namespace
    size_t last_separator = fullname.find_last_of(ECO_NAMESPACE_SEPARATOR);

    // if there is no separator, the symbol is in the root namespace
    if (last_separator == std::string::npos) {
        return find_symbol(fullname, _root);
    }

    // split the string by the last namespace separator
    std::string ns = fullname.substr(0, last_separator);
    std::string symbol_name = fullname.substr(last_separator + 2);

    // find the namespace
    return find_symbol(symbol_name, ns);
}

AST::Symbol *AST::NamespaceManager::find_symbol(const std::string &symbol_name, const std::string &ns) const
{
    auto namespace_ptr = this->get(ns);

    if (namespace_ptr == nullptr) {
        return nullptr;
    }

    return find_symbol(symbol_name, *namespace_ptr);
}

AST::Symbol *AST::NamespaceManager::find_symbol(const std::string &symbol_name, const Namespace &ns) const
{
    if (ns._symbols.find(symbol_name) == ns._symbols.end()) {
        return nullptr;
    }

    return ns._symbols.at(symbol_name).get();
}

void AST::Namespace::push_symbol(std::unique_ptr<AST::Symbol> symbol)
{
    _symbols[symbol->name()] = std::move(symbol);
}

std::string AST::Namespace::debug_dump_symbols() const
{
    std::string buffer;

    std::string name = _name.empty() ? "<root>" : _name;
    buffer = "[" + name + "]\n";

    for (const auto &symbol : _symbols) {
        buffer += "- " + symbol.first + "\n";
    }

    for (const auto &child : _children) {
        buffer += DD::tabbify(child.second->debug_dump_symbols(), 1, '|');
    }

    return buffer;
}