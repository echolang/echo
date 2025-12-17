#ifndef ASTMODULE_H
#define ASTMODULE_H

#pragma once

#include "Token.h"
#include "AST/ASTNode.h"
#include "AST/ASTFile.h"
#include "AST/ASTNodeReference.h"
#include "AST/ScopeNode.h"

#include <filesystem>
#include <functional>
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

        Module(const Module &) = delete;
        Module(Module &&) = default;


        std::string debug_description() const;

        File &add_file(const std::filesystem::path &path);

        // a TokenFilter refused the file. Thrown rather than collected, for the reason a lexer error is:
        // this happens before any AST exists, so there is no node to hang an issue off - and a file whose
        // conditional structure is broken has no meaningful parse to continue into
        struct TokenFilterException : public std::exception
        {
            std::string message;

            TokenFilterException(std::string message) : message(std::move(message)) {};

            const char *what() const noexcept override {
                return message.c_str();
            }
        };

        // applied to the tokens `file` just contributed, before its slice is measured. `from` is the index
        // the file started at, so the range is always the tail of the collection - which is what makes
        // dropping tokens inside it safe: no earlier file's slice can move.
        //
        // false with a `line N: ...` sentence stops the tokenization. See
        // Parser::filter_conditional_tokens, the only implementation
        //
        // a **callback** rather than a call, because *what* may be dropped is a Parser question and this is
        // AST. The one thing owned here is the ordering: the slice is taken after the filter, never before
        typedef std::function<bool(TokenCollection &tokens, size_t from, std::string &out_error)>
            TokenFilter;

        TokenizedFile tokenize(Lexer &lexer, const File &file, const TokenFilter &filter = nullptr);

        bool is_owner_of(const TokenReference &tokenref) const {
            return tokenref.belongs_to(tokens);
        }

        // which file did this token come from. the module-level half of the answer a token cannot give
        // about itself: the files of a module share one TokenCollection and a TokenizedFile is a slice
        // into it, so the mapping is a scan over those slices and nothing has to be stored per token.
        //
        // **null is a real answer, not a failure** - `make_virtual_token` appends past every slice, so a
        // decorated operator name or a synthesized drop's callee belongs to no file at all. A caller
        // rendering a location falls back to whatever file it was already talking about
        const File *file_of(const TokenReference &tokenref) const {
            if (!is_owner_of(tokenref)) {
                return nullptr;
            }

            const size_t handle = tokenref.get_handle();

            for (const auto &tokenized : _tokenized_files) {
                if (handle >= tokenized.token_slice.start_index && handle <= tokenized.token_slice.end_index) {
                    return tokenized.file;
                }
            }

            return nullptr;
        }

        // a token no source file spells, appended to this module's collection at the position of an
        // existing one - a decorated operator name, a closure's `closure$N`, a drop's callee
        //
        // the module owns `tokens`, so it owns minting: AST::Context and the passes that rewrite the
        // tree after parsing all reach for the same two steps, and a copy per site is a copy of "push
        // then read back the handle" that has to keep answering the same way
        TokenReference make_virtual_token(
            const std::string &value, Token::Type type, const TokenReference &at) {
            return tokens[tokens.push(value, type, at.line(), at.char_offset())];
        }

        // file iterator
        FileIterable files() { return FileIterable(_files); }

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

        // iterator
        typedef std::vector<std::unique_ptr<Module>>::iterator iterator;
        typedef std::vector<std::unique_ptr<Module>>::const_iterator const_iterator;

        iterator begin() { return _modules.begin(); }
        iterator end() { return _modules.end(); }
        const_iterator begin() const { return _modules.begin(); }
        const_iterator end() const { return _modules.end(); }
        const_iterator cbegin() const { return _modules.cbegin(); }
        const_iterator cend() const { return _modules.cend(); }

        private:
            std::vector<std::unique_ptr<Module>> _modules;
            std::unordered_map<std::string, module_handle_t> _module_map;
    };

};
#endif
