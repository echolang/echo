#include "AST/ASTNamespace.h"
#include "AST/ASTSymbol.h"

#include "Debugging.h"

#include <algorithm>
#include <utility>
#include <vector>

std::vector<std::string> split_namespace(const std::string &str)
{
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

std::vector<std::string> AST::Namespace::segments(bool display) const
{
    std::vector<std::string> segments;

    // the root carries an empty name and must not contribute a segment - and on the display walk
    // neither does a lexical namespace whose display name is empty, which is how a block nested inside
    // another block of the same function avoids rendering `outer::outer::helper`. see retrieve_lexical
    for (const Namespace *ns = this; ns != nullptr && !ns->is_root(); ns = ns->parent()) {
        if (display && ns->_display_name.empty()) {
            continue;
        }

        segments.push_back(display ? ns->_display_name : ns->_name);
    }

    std::reverse(segments.begin(), segments.end());

    return segments;
}

std::vector<std::string> AST::Namespace::path_segments() const
{
    return segments(true);
}

std::vector<std::string> AST::Namespace::mangling_segments() const
{
    // the same walk as path_segments, over `_name` rather than the display name. a lexical namespace is
    // the only place the two differ, and it is exactly where they have to: two blocks of one function
    // read as `outer` to a human, but two `outer` segments in a symbol name would put two helper
    // bodies into one llvm::Function - so one walk, one flag, and they cannot drift apart
    return segments(false);
}

const AST::Namespace *AST::Namespace::declaring_namespace() const
{
    const Namespace *ns = this;

    while (ns != nullptr && ns->_is_lexical) {
        ns = ns->_parent;
    }

    return ns;
}

AST::Namespace *AST::Namespace::declaring_namespace()
{
    return const_cast<Namespace *>(std::as_const(*this).declaring_namespace());
}

std::string AST::Namespace::full_name() const
{
    std::string buffer;

    for (const auto &segment : path_segments()) {
        if (!buffer.empty()) {
            buffer += ECO_NAMESPACE_SEPARATOR;
        }
        buffer += segment;
    }

    return buffer;
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
    for (const auto &part : parts) {
        if (current->_children.find(part) == current->_children.end()) {
            current->_children[part] = std::make_unique<Namespace>(part);
            current->_children[part]->_parent = current;
        }

        current = current->_children[part].get();
    }

    return *current;
}

AST::Namespace &AST::NamespaceManager::retrieve_lexical(
    AST::Namespace &parent, const AST::DeclarationSite &site, const std::string &display_name,
    const std::string &discriminator)
{
    if (const auto existing = parent._lexical_children.find(site); existing != parent._lexical_children.end()) {
        return *existing->second;
    }

    // a block nested inside another block of the same function displays as nothing, because a lexical
    // namespace above it already displays that function's name - without this a diagnostic for a
    // declaration two blocks deep in `outer` reads `outer::outer::helper(int32)`. the *mangling* name
    // keeps its discriminator either way, which is the half that has to stay unique
    //
    // the whole lexical chain rather than the parent alone: the parent of a third-level block displays
    // nothing itself, so asking only it would let the name back in at every odd depth
    bool ancestor_already_displays = false;
    for (const Namespace *ns = &parent; ns != nullptr && ns->_is_lexical; ns = ns->_parent) {
        if (ns->_display_name == display_name) {
            ancestor_already_displays = true;
            break;
        }
    }

    // `outer$mainL4C5` mangles, `outer` displays. the discriminator is what keeps two blocks of one
    // function apart in a symbol name, and leaving it out of the display name is what keeps it out of
    // every diagnostic - a user never wrote this namespace and should never have to read its position
    auto lexical = std::make_unique<Namespace>(display_name + "$" + discriminator);
    lexical->_display_name = ancestor_already_displays ? "" : display_name;
    lexical->_is_lexical = true;
    lexical->_parent = &parent;

    auto &inserted = *lexical;
    parent._lexical_children[site] = std::move(lexical);

    return inserted;
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
    for (const auto &part : parts) {
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
    // one lookup, not a find followed by an at: find_symbol_in_scope calls this once per ancestor
    // namespace, and every unqualified type name in every module pass goes through that walk
    const auto it = ns._symbols.find(symbol_name);

    if (it == ns._symbols.end()) {
        return nullptr;
    }

    return it->second.get();
}

AST::Symbol *AST::NamespaceManager::find_symbol_in_scope(
    const std::string &symbol_name, const AST::Namespace &from) const
{
    // innermost first, exactly as FunctionRegistry::overloads walks - the nearest namespace that
    // declares the name answers, and an outer one is hidden by it rather than consulted as well.
    // a lexical namespace holds no type symbols, so the walk simply passes through one
    for (const Namespace *current = &from; current != nullptr; current = current->parent()) {
        if (AST::Symbol *found = find_symbol(symbol_name, *current)) {
            return found;
        }
    }

    return nullptr;
}

void AST::Namespace::push_symbol(std::unique_ptr<AST::Symbol> symbol)
{
    _symbols[symbol->name()] = std::move(symbol);
}

std::string AST::Namespace::debug_dump_symbols() const
{
    std::string buffer;

    // the unique name rather than the display one: two sibling blocks of a function would otherwise
    // print as two indistinguishable `[outer]` blocks, and a dump whose entries cannot be told apart
    // is worse than a slightly noisy one
    std::string name = _name.empty() ? "<root>" : _name;
    buffer = "[" + name + "]\n";

    for (const auto &symbol : _symbols) {
        buffer += "- " + symbol.first + "\n";
    }

    for (const auto &child : _children) {
        buffer += DD::tabbify(child.second->debug_dump_symbols(), 1, '|');
    }

    // a lexical namespace holds no type symbols today - functions live in AST::FunctionRegistry - so
    // one with nothing under it would be pure noise in every dump of every program that has a block
    for (const auto &child : _lexical_children) {
        if (child.second->_symbols.empty() && child.second->_children.empty()
            && child.second->_lexical_children.empty()) {
            continue;
        }

        buffer += DD::tabbify(child.second->debug_dump_symbols(), 1, '|');
    }

    return buffer;
}