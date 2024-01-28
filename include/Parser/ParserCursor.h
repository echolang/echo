#ifndef PARSERCURSOR_H
#define PARSERCURSOR_H

#pragma once

#include <string>
#include <assert.h>
#include "../Token.h"

namespace Parser
{
    class Cursor
    {
        size_t _index = 0;
        size_t _end = 0;

    public:
        const TokenCollection &tokens;

        Cursor(const TokenCollection &tokens, size_t start = 0, size_t end = 0) : 
            tokens(tokens), _index(start), _end(end) 
        {}
        ~Cursor() {};

        inline size_t range_size() const {
            return _end > 0 ? _end : tokens.size();
        }

        inline bool is_done() const {
            return _index >= range_size();
        }

        inline bool is_valid_offset(size_t offset) const {
            return _index + offset < range_size();
        }

        inline TokenReference current() const {
            assert(_index < range_size());
            return tokens[_index];
        }
        
        inline TokenReference peek(size_t offset = 1) const {
            assert(is_valid_offset(offset));
            return tokens[_index + offset];
        }

        inline void skip(size_t offset = 1) {
            _index += offset;
            _index = std::min(_index, range_size());
        }

        inline void skip_until(Token::Type type) {
            while (!is_done() && current().type() != type) {
                skip();
            }
        }

    private:

    };
};



#endif