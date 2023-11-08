#ifndef PARSERCURSOR_H
#define PARSERCURSOR_H

#pragma once

#include "../Token.h"

namespace Parser
{
    class Cursor
    {
        size_t _index = 0;

    public:
        const TokenCollection &tokens;

        Cursor(const TokenCollection &tokens) : tokens(tokens) {}
        ~Cursor() {};

        inline bool is_done() const {
            return _index >= tokens.tokens.size();
        }

        inline bool is_valid_offset(size_t offset) const {
            return _index + offset < tokens.tokens.size() && _index + offset >= 0;
        }

        inline TokenReference current() const {
            assert(_index < tokens.tokens.size());
            return tokens[_index];
        }
        
        inline TokenReference peek(size_t offset = 1) const {
            assert(is_valid_offset(offset));
            return tokens[_index + offset];
        }

        inline void skip(size_t offset = 1) {
            _index += offset;
            _index = std::min(_index, tokens.tokens.size());
        }

    private:

    };
};



#endif