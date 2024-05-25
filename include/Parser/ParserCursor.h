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
        struct Snapshot {
            size_t index;
            size_t end;
        };

        const TokenCollection &tokens;

        Cursor(const TokenCollection &tokens, size_t start = 0, size_t end = 0) : 
            tokens(tokens), _index(start), _end(end) 
        {}
        ~Cursor() {};

        inline Snapshot snapshot() const {
            return { _index, _end };
        }

        inline void restore(const Snapshot &snapshot) {
            _index = snapshot.index;
            _end = snapshot.end;
        }

        // returns a slices from the given start and end snapshot
        TokenSlice slice(const Snapshot &start, const Snapshot &end) const;

        inline size_t range_size() const {
            return _end > 0 ? _end : tokens.size();
        }

        inline bool is_done() const {
            return _index >= range_size();
        }

        inline bool is_valid(size_t index) const {
            return index < range_size();
        }

        inline bool is_valid_offset(size_t offset) const {
            return is_valid(_index + offset);
        }

        inline TokenReference current() const {
            assert(_index < range_size());
            return tokens[_index];
        }
        
        inline TokenReference peek(size_t offset = 1) const {
            assert(is_valid_offset(offset));
            return tokens[_index + offset];
        }

        inline const Token::Type type(size_t index) const {
            if (!is_valid(index)) return Token::Type::t_unknown;
            return tokens.tokens[index].type;
        }

        inline const Token::Type type() const {
            return type(_index);
        }

        inline const Token::Type peek_type(size_t offset) const {
            return type(_index + offset);
        }

        inline const bool is_type_at(size_t index, const Token::Type as_type) const {
            return type(index) == as_type;
        }

        inline const bool is_type(const Token::Type as_type) const {
            return is_type_at(_index, as_type);
        }

        const bool is_type(std::initializer_list<Token::Type> types) const {
            for (auto type : types) {
                if (is_type_at(_index, type)) {
                    return true;
                }
            }
            return false;
        }

        inline const bool peek_is_type(size_t offset, const Token::Type as_type) const {
            return is_type_at(_index + offset, as_type);
        }

        inline const bool is_type_sequence(size_t offset, std::initializer_list<Token::Type> types) const {
            for (auto type : types) {
                if (!is_type_at(_index + offset, type)) {
                    return false;
                }
                offset++;
            }
            return true;
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

        void skip_until(std::initializer_list<Token::Type> types);

        // this function is usally called after an error has been detected
        // it will skip until the next statement or block is found to continue parsing
        void try_skip_to_next_statement();


    private:

    };
};



#endif