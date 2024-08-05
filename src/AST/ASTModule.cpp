#include "AST/ASTModule.h"
#include "Debugging.h"
#include "Lexer.h"

std::string AST::Module::debug_description() const
{
    std::string result = "[" + name + "]\n";
    for (auto &file : _files) {
        result += "File<" + file->get_path().string() + ">\n{\n";
        result += DD::tabbify(file->debug_description(), 2) + "\n";
        result += "}\n";
    }
    return result;
}

AST::File &AST::Module::add_file(const std::filesystem::path &path)
{
    auto file_index = _files.size();
    _files.push_back(std::make_unique<File>(path));

    auto &file = *_files[file_index].get();

    file.module = this;

    _tokenized_files.reserve(_files.size());

    return file;
}

AST::TokenizedFile AST::Module::tokenize(Lexer &lexer, const AST::File &file)
{
    // throw an error if the file content is not available
    if (!file.content.has_value()) {
        throw std::runtime_error("Cannot tokenize a file without content");
    }

    // sanity check that the given file is actually allocated in this module
    if (file.module != this) {
        throw std::runtime_error("Cannot tokenize a file that is not in this module");
    }

    AST::OperatorRegistry ops;
    lexer.tokenize_prepass_operators(file.content.value(), ops);

    size_t startindex = tokens.size();
    lexer.tokenize(tokens, file.content.value(), &ops);
    size_t endindex = tokens.size();

    _tokenized_files.push_back(TokenizedFile {
        .file = &file,
        .token_slice = tokens.slice(startindex, endindex)
    });

    return _tokenized_files.back();
}

AST::module_handle_t AST::ModuleCollection::add_module(const std::string &name)
{
    auto handle = _modules.size();
    _modules.push_back(std::make_unique<Module>(name, handle));
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