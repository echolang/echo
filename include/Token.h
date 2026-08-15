#ifndef TOKEN_H
#define TOKEN_H

#pragma once

#include <vector>
#include <string>
#include <cassert>
#include <initializer_list>

#include <cstdint>

namespace AST { class File; };

struct Token
{
public:
    enum class Type
    {
        t_identifier,
        t_semicolon,                // ;
        t_colon,                    // :
        t_ptr_of,                   // :$ - the address of a pointer expression
        t_comma,                    // ,
        t_dot,                      //
        t_logical_and,              // &&
        t_logical_or,               // ||
        t_logical_eq,               // ==
        t_logical_neq,              // !=
        t_logical_leq,              // <=
        t_logical_geq,              // >=
        t_accessorlr,               // ->
        t_assign,                   // =
        t_double_arrow,             // =>
        t_and,                      // &
        t_ref,                      // & (reference)
        t_or,                       // |
        t_xor,                      // ^
        t_op_shl,                   // <<
        t_op_shr,                   // >>
        t_op_inc,                   // ++
        t_op_dec,                   // --
        t_op_add,                   // +
        t_op_sub,                   // -
        t_op_mul,                   // *
        t_op_div,                   // /
        t_op_mod,                   // %
        t_op_pow,                   // **
        t_op_custom,                // <custom>
        t_qmark,                    // ?
        t_qmark_qmark,              // ??
        t_optional_arrow,           // ?->
        t_exclamation,              // !
        t_open_angle,               // <
        t_close_angle,              // >
        t_open_paren,               // (
        t_close_paren,              // )
        t_open_brace,               // {
        t_close_brace,              // }
        t_open_bracket,             // [
        t_close_bracket,            // ]
        t_hash,                     // #
        t_string_literal,           // "..." or '...', quotes kept, escapes undecoded

        // the four an interpolated `"..."` lexes to. the three chunk types carry the raw text
        // *between* holes, unquoted; the hole's own tokens sit between them and are ordinary Echo.
        // so `"a{$x:>4}b"` is begin("a") varname($x) spec(">4") end("b")
        t_string_interp_begin,      // the chunk before the first hole
        t_string_interp_middle,     // the chunk between two holes
        t_string_interp_end,        // the chunk after the last hole
        t_string_interp_spec,       // a hole's format spec, the raw text after its top level `:`

        t_integer_literal,          // 123
        t_hex_literal,              // 0x123
        t_binary_literal,           // 0b101
        t_floating_literal,         // 123.456
        t_bool_literal,             // true, false
        t_varname,                  // $varname
        t_const,                    // const
        t_echo,                     // echo
        t_function,                 // function
        t_return,                   // return
        t_if,                       // if
        t_guard,                    // guard
        t_else,                     // else
        t_while,                    // while
        t_for,                      // for
        t_foreach,                  // foreach
        t_break,                    // break
        t_continue,                 // continue
        t_namespace,                // namespace
        t_namespace_sep,            // ::
        t_ptr,                      // ptr
        t_weak,                     // weak
        t_strong,                   // strong
        t_null,                     // null
        t_struct,                   // struct
        t_class,                    // class
        t_interface,                // interface
        t_enum,                     // enum
        t_case,                     // case
        t_match,                    // match
        t_extern,                   // extern
        t_as,                       // as
        t_destructor,               // destructor
        t_instanceof,               // instanceof
        t_mv,                       // mv
        t_unsafe,                   // unsafe
        t_private,                  // private
        t_internal,                 // internal
        t_public,                   // public
        t_operator,                 // operator
        t_test,                     // test
        t_static,                   // static
        t_unknown
    };

    Type type;
    uint32_t line;
    uint32_t char_offset; // if you have a file source file thats 2GB, you're have other problems

    // the missing third of a source location. line and column were always here; the file was a
    // slice scan on the module, then a walk over every module in the bundle. a token that names
    // its own file needs neither. incomplete type: this header must not include AST
    AST::File *file = nullptr;

    // this compiler invented the token rather than lexing it - a `$__it`, a decorated operator
    // name, a synthesized drop's callee. **not** "file is null": a minted token inherits `at`'s
    // file so a location can still name a line, and a token another module owns has a file too
    bool minted = false;

    Token(Type type, uint32_t line, uint32_t char_offset, AST::File *file = nullptr, bool minted = false)
        : type(type), line(line), char_offset(char_offset), file(file), minted(minted) {}

    inline bool is_a(Type type) const {
        return this->type == type;
    }

    inline bool is_one_of(std::initializer_list<Type> types) const {
        for (auto type : types) {
            if (this->type == type) {
                return true;
            }
        }
        return false;
    }

    bool is_operator_type() const;
};

// function to convert token type to string
// mostly used for debugging purposes
const std::string token_type_string(Token::Type type);

// returns a string representation of the literal symbol
// used for lexing and parsing, only available for literals
const std::string token_lit_symbol_string(const Token::Type type);

// **is this token's spelling a bare word** - `test`, `darwin`, `size_of`, `if`.
//
// asked by the two grammars that read a word where no declaration can follow, and it has to be one answer
// for both: an attribute's *name*, which Parser::filter_conditional_tokens reads to find its directives, and
// an attribute's *value*, where AST::AttributeValueKind::t_name means itself. `#[if: os == darwin]` and
// `#[target: test]` each hold a word Echo also keeps as a keyword, and inside `#[ ]` there is nothing for a
// keyword's meaning to collide with - the region is compile-time data and resolves to no declaration.
//
// answered from the **spelling** rather than from a list of keyword types, so a keyword added to the lexer is
// a word here with no edit: a list would be a second registry that goes stale, and its going stale reads as
// an attribute value being refused for no reason a user could see
bool token_spells_a_word(const std::string &value);

class TokenReference;
struct TokenSlice;
struct TokenCollection
{

    std::vector<Token> tokens;
    std::vector<std::string> token_values;

    // stamped onto every `push` that is not minted. Module::tokenize sets this to the file it is
    // about to lex, then clears it; lexer tests leave it null
    AST::File *appending_file = nullptr;

    size_t push(const std::string &value, Token::Type type, size_t line, size_t char_offset) {
        tokens.emplace_back(type, line, char_offset, appending_file, false);
        token_values.push_back(value);
        return tokens.size() - 1;
    }

    // a token no source file spells, at the position (and file) of an existing one
    size_t push_minted(
        const std::string &value,
        Token::Type type,
        uint32_t line,
        uint32_t char_offset,
        AST::File *file
    ) {
        tokens.emplace_back(type, line, char_offset, file, true);
        token_values.push_back(value);
        return tokens.size() - 1;
    }

    void clear() {
        tokens.clear();
        token_values.clear();
        appending_file = nullptr;
    }

    inline size_t size() const {
        return tokens.size();
    }

    TokenReference operator[](size_t index) const;

    TokenSlice slice(size_t start, size_t end) const;
};

class TokenReference
{
    const TokenCollection &tokens;
    size_t index;

public:

    TokenReference(const TokenCollection &tokens, size_t index)
        : tokens(tokens), index(index)
    {}

    TokenReference(TokenCollection *tokens, size_t index)
        : tokens(*tokens), index(index)
    {}

    inline bool belongs_to(const TokenCollection &tokens) const {
        return &this->tokens == &tokens;
    }

    // returns the handle to the token inside of its collection
    // keep in mind that token and its value is owned by the collection
    // use carefully
    inline size_t get_handle() const {
        return index;
    }

    inline const TokenCollection &get_collection_ref() const {
        return tokens;
    }

    inline bool is_valid() const {
        return index < tokens.tokens.size();
    }

    inline const std::string &value() const {
        assert(is_valid());
        return tokens.token_values[index];
    }

    inline const Token &token() const {
        assert(is_valid());
        return tokens.tokens[index];
    }

    inline Token::Type type() const {
        assert(is_valid());
        return tokens.tokens[index].type;
    }

    inline uint32_t line() const {
        assert(is_valid());
        return tokens.tokens[index].line;
    }

    inline uint32_t char_offset() const {
        assert(is_valid());
        return tokens.tokens[index].char_offset;
    }

    // same as char_offset..
    inline uint32_t column() const {
        assert(is_valid());
        return tokens.tokens[index].char_offset;
    }

    // which file this lexeme was written in, or null if nothing stamped one - a standalone
    // lexer test, a mint from a no-file `at`
    inline AST::File *file() const {
        assert(is_valid());
        return tokens.tokens[index].file;
    }

    // did this compiler invent the token rather than lex it
    inline bool is_minted() const {
        assert(is_valid());
        return tokens.tokens[index].minted;
    }

    inline TokenReference next() const {
        return TokenReference(tokens, index + 1);
    }

    inline TokenReference prev() const {
        return TokenReference(tokens, index - 1);
    }

    TokenSlice make_slice(size_t offset = 0) const;

    inline bool operator==(const TokenReference &other) const {
        return &tokens == &other.tokens && index == other.index;
    }

    inline bool operator!=(const TokenReference &other) const {
        return !(*this == other);
    }
};

struct TokenSlice
{
    const TokenCollection &tokens;
    const size_t start_index;
    const size_t end_index;

    const TokenReference start_ref() const;
    const TokenReference end_ref() const;

    const Token &startt() const {
        return tokens.tokens[start_index];
    }

    const Token &endt() const {
        return tokens.tokens[end_index];
    }

    // returns the N'th token inside of the slice
    TokenReference operator[](size_t index) const {
        assert(start_index + index <= end_index);
        return TokenReference(tokens, start_index + index);
    }

    bool is_empty() const {
        return start_index == end_index;
    }

    size_t size() const {
        return end_index - start_index;
    }

    bool valid_index(size_t index) const {
        return start_index + index < end_index;
    }

    struct iterator
    {
        const TokenSlice &slice;
        size_t index;

        iterator(const TokenSlice &slice, size_t index)
            : slice(slice), index(index) {}

        TokenReference operator*() const {
            return TokenReference(slice.tokens, index);
        }

        iterator &operator++() {
            index++;
            return *this;
        }

        bool operator!=(const iterator &other) const {
            return index != other.index;
        }
    };

    iterator begin() const {
        return iterator(*this, start_index);
    }

    iterator end() const {
        return iterator(*this, end_index);
    }
};

// a token range, as three plain values.
//
// **TokenSlice with the const stripped off**, and that is the whole of why it exists: a slice holds a
// reference and two const members, so it can be neither assigned nor stored in a container that has to
// grow - and an attribute value has to live in a std::vector and in a std::optional at once. Everything
// that wants a slice asks for one; nothing stores the slice itself.
//
// **`end_index` is inclusive, exactly as TokenSlice's is** - a span of one token has start == end, which
// is what AST::span_of draws an underline from. The two factories below are the only places that
// convention is spelled, because a cursor position is one *past* the last token consumed and converting
// it by hand at each site is one off-by-one per site.
//
// an invalid span is a real answer - a value carries one for the tag it does not have
struct TokenSpan
{
    const TokenCollection *tokens = nullptr;
    size_t start_index = 0;
    size_t end_index = 0;

    // one token
    static TokenSpan of(const TokenReference &token) {
        return TokenSpan { &token.get_collection_ref(), token.get_handle(), token.get_handle() };
    }

    // everything a walk consumed, given where it started and where the cursor now is. `past_end` is a
    // cursor index, so the last token it holds is the one before it
    static TokenSpan upto(const TokenCollection &tokens, size_t start, size_t past_end) {
        return TokenSpan { &tokens, start, past_end > start ? past_end - 1 : start };
    }

    bool is_valid() const {
        return tokens != nullptr && end_index >= start_index && start_index < tokens->size();
    }

    TokenSlice slice() const {
        assert(is_valid());
        return tokens->slice(start_index, end_index);
    }

    TokenReference first() const {
        assert(is_valid());
        return TokenReference(*tokens, start_index);
    }

    // the line a hand-formatted message points at - the manifest reader's `<file>:<line>: <what>`,
    // which has no renderer to hand it a slice. zero when there is nothing to point at
    uint32_t line() const {
        return is_valid() ? first().line() : 0;
    }
};

struct TokenList
{
    const TokenCollection &tokens;
    std::vector<size_t> indices;

    TokenList(const TokenCollection &tokens)
        : tokens(tokens) {}

    TokenReference operator[](size_t index) const {
        return TokenReference(tokens, indices[index]);
    }

    void push(size_t index) {
        indices.push_back(index);
    }

    void push(const TokenReference &ref) {
        assert(ref.belongs_to(tokens));
        indices.push_back(ref.get_handle());
    }

    // iterator for the token list
    struct iterator
    {
        const TokenList &list;
        size_t index;

        iterator(const TokenList &list, size_t index)
            : list(list), index(index) {}

        TokenReference operator*() const {
            return TokenReference(list.tokens, list.indices[index]);
        }

        iterator &operator++() {
            index++;
            return *this;
        }

        bool operator!=(const iterator &other) const {
            return index != other.index;
        }
    };

    iterator begin() const {
        return iterator(*this, 0);
    }

    iterator end() const {
        return iterator(*this, indices.size());
    }
};

#endif
