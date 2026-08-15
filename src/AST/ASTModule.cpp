#include "AST/ASTModule.h"
#include "Debugging.h"
#include "Lexer.h"

#include <stdexcept>

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

AST::TokenizedFile AST::Module::tokenize(
    Lexer &lexer,
    AST::File &file,
    const AST::Module::TokenFilter &filter
)
{
    // throw an error if the file content is not available
    if (!file.content.has_value()) {
        throw std::runtime_error("Cannot tokenize a file without content");
    }

    // sanity check that the given file is actually allocated in this module
    if (file.module != this) {
        throw std::runtime_error("Cannot tokenize a file that is not in this module");
    }

    // the lexer knows nothing about custom operators, deliberately. a declared symbol is a
    // *sequence of ordinary tokens* that AST::OperatorRegistry::match_at recognises in the parser,
    // so there is nothing to pre-scan here and nothing to re-lex - which is also what lets a
    // symbol declared in one file be used in another, since the operator table is filled by the
    // module's first parse pass rather than per file at lex time
    size_t startindex = tokens.size();

    // the collection stamps this onto every push; a throw from the lexer must not leave it set
    struct AppendingFile
    {
        TokenCollection &tokens;

        AppendingFile(TokenCollection &tokens, AST::File *file) : tokens(tokens)
        {
            tokens.appending_file = file;
        }

        ~AppendingFile() {
            tokens.appending_file = nullptr;
        }
    };

    AppendingFile stamp(tokens, &file);
    lexer.tokenize(tokens, file.content.value());

    // **between lexing and the slice**, which is the whole of why the filter is a parameter here rather
    // than something a caller does afterwards: a slice measured first and filtered second would name
    // tokens that are no longer at those indices
    if (filter) {
        std::string error;

        if (!filter(tokens, startindex, error)) {
            throw TokenFilterException(error);
        }
    }

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
