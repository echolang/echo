#ifndef PARSERCURSOR_H
#define PARSERCURSOR_H

#pragma once

#include <string>
#include <algorithm>
#include <initializer_list>
#include <assert.h>
#include "Token.h"

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
        struct Snapshot
        {
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

        // how many tokens are left in *this* range. a module's TokenCollection holds every one of its
        // files, so anything walking forward token by token has to be handed this bound rather than
        // trusting TokenReference::next() - which would happily run into the next file
        inline size_t remaining() const {
            return is_done() ? 0 : range_size() - _index;
        }

        inline bool is_empty() const {
            return range_size() == 0;
        }

        // the last token of the range. what a diagnostic about *running out* of input points at, since
        // there is no current token to name - see Payload::collect_unexpected_token
        inline TokenReference last() const {
            assert(!is_empty());
            return tokens[range_size() - 1];
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
                    // second '>' of this '>>' - done with it
                    _shr_split_index = (size_t)-1;
                    skip();
                } else {
                    // first '>' of this '>>' - leave the token in place for the outer level
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

        // will skip to the end of the current scope / block, brace-depth aware, and *does* consume the
        // scope terminating "}" closing brace token - so the cursor lands on the token after the block
        // Note: This function assumes that you are already inside of a scope
        void skip_till_end_of_scope();

        // steps over a balanced `open ... close` group **from its opening token**, landing on the
        // token after the closing one. does nothing when the cursor is not on an opener
        //
        // unlike skip_till_end_of_scope, which starts inside its scope, this is the "walk past a group
        // I am not parsing" form - a declaration reader stepping over an operand list it will come
        // back to. one function for both bracket kinds: the loop is the same and two copies of a
        // depth count are two places to get it wrong
        void skip_balanced_group(Token::Type open, Token::Type close);

        // this function is usally called after an error has been detected
        // it will skip until the next statement or block is found to continue parsing
        //
        // **recovery may end a statement, never a scope.** it stops at the next `;` or `}` and
        // consumes only the first: a closing brace belongs to whichever parser opened it, which is
        // sitting one frame up waiting for that very token. so this lands either *after* a
        // terminator or *on* a brace, and a caller's loop sees the block end where it really is
        void try_skip_to_next_statement();

        // steps over a whole statement from its first token, landing after its `;` - stepping *over* any
        // braced group on the way, so a value that contains one (a closure literal) is not mistaken for
        // the statement's end
        //
        // deliberately not try_skip_to_next_statement, and the difference is which of the two is a guess.
        // that one is error *recovery*: the statement is already known to be malformed, so the first
        // terminator it can see is the best it can do, and stepping over a brace it has no reason to trust
        // would jump out of the block the error was in. this one is asked about a statement a *different
        // pass already parsed successfully*, so its shape is known-good and the whole of it is skippable
        void skip_statement();


    private:

    };
};



#endif