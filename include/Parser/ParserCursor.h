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

        // index of a '>>' (t_op_shr) token whose first '>' has already been consumed as a
        // generic close; the second '>' is still pending at that same index. (size_t)-1 = none
        size_t _shr_split_index = (size_t)-1;

    public:
        struct Snapshot {
            size_t index;
            size_t end;
            size_t shr_split_index;
        };

        const TokenCollection &tokens;

        Cursor(const TokenCollection &tokens, size_t start = 0, size_t end = 0) :
            _index(start), _end(end), tokens(tokens)
        {}
        ~Cursor() {};

        inline Snapshot snapshot() const {
            return { _index, _end, _shr_split_index };
        }

        inline void restore(const Snapshot &snapshot) {
            _index = snapshot.index;
            _end = snapshot.end;
            _shr_split_index = snapshot.shr_split_index;
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

        inline Token::Type type(size_t index) const {
            if (!is_valid(index)) return Token::Type::t_unknown;
            return tokens.tokens[index].type;
        }

        inline Token::Type type() const {
            return type(_index);
        }

        inline Token::Type peek_type(size_t offset) const {
            return type(_index + offset);
        }

        inline bool is_type_at(size_t index, const Token::Type as_type) const {
            return type(index) == as_type;
        }

        inline bool is_type(const Token::Type as_type) const {
            return is_type_at(_index, as_type);
        }

        bool is_type(std::initializer_list<Token::Type> types) const {
            for (auto type : types) {
                if (is_type_at(_index, type)) {
                    return true;
                }
            }
            return false;
        }

        inline bool peek_is_type(size_t offset, const Token::Type as_type) const {
            return is_type_at(_index + offset, as_type);
        }

        inline bool is_type_sequence(size_t offset, std::initializer_list<Token::Type> types) const {
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

        // true when the current token can close a single generic level: a plain '>'
        // (t_close_angle) or a '>>' (t_op_shr), the latter closing one level and leaving
        // a '>' for the enclosing level (the standard C++ nested-generics token split)
        inline bool is_generic_close() const {
            return is_type(Token::Type::t_close_angle) || is_type(Token::Type::t_op_shr);
        }

        // consume a single closing '>' of a generic argument list. a '>>' is split in place:
        // the first call consumes one '>' without advancing (leaving the token for the outer
        // level), the second call at the same token fully consumes it
        inline void consume_generic_close() {
            if (is_type(Token::Type::t_op_shr)) {
                if (_shr_split_index == _index) {
                    // second '>' of this '>>' — done with it
                    _shr_split_index = (size_t)-1;
                    skip();
                } else {
                    // first '>' of this '>>' — leave the token in place for the outer level
                    _shr_split_index = _index;
                }
                return;
            }

            skip(); // plain '>'
        }

        inline void skip_until(Token::Type type) {
            while (!is_done() && current().type() != type) {
                skip();
            }
        }

        void skip_until(std::initializer_list<Token::Type> types);

        // will skip to the end of the current scope / block, 
        // will not skip the scope terminating "}" closing brace token 
        // Note: This function assumes that you are already inside of a scope
        void skip_till_end_of_scope();

        // this function is usally called after an error has been detected
        // it will skip until the next statement or block is found to continue parsing
        void try_skip_to_next_statement();


    private:

    };
};



#endif