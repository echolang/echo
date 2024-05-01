#include "AST/ASTModule.h"
#include "Debugging.h"

std::string AST::Module::debug_description() const
{
    std::string result = "[Module]\n";
    for (auto &file : files) {
        result += "File<" + file.get_path().string() + ">\n{\n";
        result += DD::tabbify(file.debug_description(), 2) + "\n";
        result += "}\n";
    }
    return result;
}

AST::module_handle_t AST::ModuleCollection::add_module(const std::string &name)
{
    _modules.push_back(std::make_unique<Module>(name));
    auto handle = _modules.size() - 1;
    _module_map[name] = handle;
    return handle;
}

AST::Module *AST::ModuleCollection::find_module_ptr(const std::string &name)
{
    auto it = _module_map.find(name);
    if (it == _module_map.end()) {
        return nullptr;
    }
    return _modules[it->second].get();
}

AST::Module &AST::ModuleCollection::find_module(const std::string &name)
{
    assert(has_module(name));
    return *find_module_ptr(name);
}

bool AST::ModuleCollection::has_module(const std::string &name)
{
    return find_module_ptr(name) != nullptr;
}