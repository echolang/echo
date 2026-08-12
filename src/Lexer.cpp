#include "Lexer.h"

#include <algorithm>
#include <cassert>

constexpr bool is_hex_char(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

constexpr bool is_numeric_char(char c)
{
    return (c >= '0' && c <= '9');
}

constexpr bool is_valid_varname_char(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           (c == '_');
}

constexpr std::array<bool, 256> generate_hex_lut()
{
    std::array<bool, 256> table = {};
    for (size_t i = 0; i < 256; ++i) {
        table[i] = is_hex_char(static_cast<char>(i));
    }
    return table;
}

constexpr std::array<bool, 256> generate_numeric_lut()
{
    std::array<bool, 256> table = {};
    for (size_t i = 0; i < 256; ++i) {
        table[i] = is_numeric_char(static_cast<char>(i));
    }
    return table;
}

constexpr std::array<bool, 256> generate_varname_lut()
{
    std::array<bool, 256> table = {};
    for (size_t i = 0; i < 256; ++i) {
        table[i] = is_valid_varname_char(static_cast<char>(i));
    }
    return table;
}

constexpr auto hex_lut = generate_hex_lut();
constexpr auto numeric_lut = generate_numeric_lut();
constexpr auto varname_lut = generate_varname_lut();

void insert_function_into_tree(LexerFunction::TreeNode &root, const std::string &must_match, LexerFunction::Base *func, size_t prefix_limit = 3)
{
    auto match_limit = std::min<size_t>(prefix_limit, must_match.size());
    auto node = &root;
    for (size_t i = 0; i < match_limit; ++i) {
        auto c = must_match[i];
        if (node->children.find(c) == node->children.end()) {
            node->children[c] = std::make_unique<LexerFunction::TreeNode>(must_match.substr(0, i + 1));
        }

        node = node->children[c].get();
    }

    node->functions.push_back(func);
}

void sort_functiontree(LexerFunction::TreeNode &node)
{
    for (auto &child : node.children) {
        sort_functiontree(*child.second);
    }

    std::sort(node.functions.begin(), node.functions.end(), [](const auto &a, const auto &b) {
        return a->priority() > b->priority();
    });
}

LexerEngine::LexerEngine(FunctionList &functions)
{
    // we build a tree like structure based on the "must_match" string of each function
    // we simply take the first N chars of the must match
    // im sure there is a better way to do this but works for now
    _root = std::make_unique<LexerFunction::TreeNode>("root");

    for (auto &func : functions) {
        auto machter_stringns = func->must_match();
        for (auto &match : machter_stringns) {
            insert_function_into_tree(*_root.get(), match, func.get(), MAX_PREFIX_LENGTH);
        }
    }

    sort_functiontree(*_root.get());

    // **after the trie, and that order is the whole reason this is a call rather than a
    // constructor argument**: the function being handed the engine is one of the functions the
    // trie was just built out of
    for (auto &func : functions) {
        func->bind_engine(*this);
    }
}

bool LexerEngine::step(TokenCollection &tokens, LexerCursor &cursor) const
{
    auto node = _root.get();
    auto peek_offset = 0;
    while (!cursor.is_eof() && node->children.find(cursor.peek(peek_offset)) != node->children.end()) {
        node = node->children[cursor.peek(peek_offset)].get();
        peek_offset++;
    }

    // the trie's own functions first, then the root's - and **the root's exactly once**, where a leading
    // "this node has none, so try the root" block ran them a second time for every character the trie
    // walked to a node that declares nothing
    for (auto &func : node->functions) {
        if (func->parse(tokens, cursor)) {
            return true;
        }
    }

    // if still nothing matched retry with the root functions
    for (auto &func : _root->functions) {
        if (func->parse(tokens, cursor)) {
            return true;
        }
    }

    return false;
}

void LexerEngine::run(TokenCollection &tokens, LexerCursor &cursor) const
{
    while (!cursor.is_eof()) {
        // formatting aka whitespace, tabs, newlines
        if (cursor.is_formatting()) {
            cursor.skip_formatting();
            if (cursor.is_eof()) {
                break;
            }
        }

        if (!step(tokens, cursor)) {
            throw Lexer::UnknownTokenException("Unexpected", cursor.line, cursor.char_offset);
        }
    }
}


#define ECHO_LEX_MAKE_FNCLIST(name) \
    std::vector<std::unique_ptr<LexerFunction::Base>> name;

#define ECHO_LEX_FNC_CHAR(name, type) \
    assert(token_lit_symbol_string(type).length() == 1 && "Char token must have a length of 1"); \
    name.push_back(std::make_unique<LexerFunction::CharToken>(token_lit_symbol_string(type).at(0), type));

#define ECHO_LEX_FNC_STRING(name, type) \
    name.push_back(std::make_unique<LexerFunction::StringToken>(token_lit_symbol_string(type), type));

#define ECHO_LEX_FNC_KEYWORD(name, type) \
    name.push_back(std::make_unique<LexerFunction::KeywordToken>(token_lit_symbol_string(type), type));

#define ECHO_LEX_FNC_CUST_KEYWORD(name, lit, type) \
    name.push_back(std::make_unique<LexerFunction::KeywordToken>(lit, type));

void Lexer::tokenize(TokenCollection &tokens, const std::string &input)
{
    auto cursor = LexerCursor(input);

    // build a list of lexer functions
    ECHO_LEX_MAKE_FNCLIST(lx_functions);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_semicolon);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_colon);
    // `:$` reaches the pointer itself. a StringToken's priority is its length, so the two
    // character match wins over the plain `:` sharing the trie's ':' node, and `::` keeps its
    // own deeper node. the trailing '$' is what keeps `$pp:$:$` spellable - a bare `:` would
    // lex two of them as the namespace separator
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_ptr_of);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_comma);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_dot);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_accessorlr);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_logical_and);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_logical_or);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_logical_eq);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_logical_neq);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_logical_leq);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_logical_geq);
    // `=>` before the bare `=`. a StringToken's priority is its length and the trie descends to the
    // deepest match, the same rule that keeps `?->` from lexing as `??` - and `=` immediately followed
    // by `>` is unspellable in Echo otherwise, so this cannot reinterpret an existing program
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_double_arrow);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_assign);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_and);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_or);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_xor);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_op_inc);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_op_dec);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_op_shl);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_op_shr);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_op_add);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_op_sub);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_op_mul);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_op_div);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_op_mod);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_op_pow);
    // `?->` and `??` before the bare `?`. a StringToken's priority is its length, so the trie picks the
    // longest match at a shared prefix - the same rule that keeps `:$` from lexing as a bare `:`
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_optional_arrow);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_qmark_qmark);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_qmark);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_exclamation);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_open_angle);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_close_angle);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_open_paren);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_close_paren);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_open_brace);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_close_brace);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_open_bracket);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_close_bracket);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_hash);

    // every keyword spelled out of identifier characters goes in as a KEYWORD rather than a
    // STRING: a plain StringToken matches a prefix, so `const` ate the head of `constructor` and
    // `for` the head of `forward`. the keyword variant is the same trie entry plus a one
    // character look-ahead for a word boundary
    ECHO_LEX_FNC_CUST_KEYWORD(lx_functions, "true", Token::Type::t_bool_literal);
    ECHO_LEX_FNC_CUST_KEYWORD(lx_functions, "false", Token::Type::t_bool_literal);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_const);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_echo);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_function);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_return);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_if);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_else);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_guard);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_while);
    // `foreach` before `for`, though the order here does not decide it: insert_function_into_tree
    // truncates at three characters so both land in the "for" node, and sort_functiontree orders by
    // priority descending - a KeywordToken's priority is its length, so `foreach` is tried first and
    // `for(` still falls through to `for`
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_foreach);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_for);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_break);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_continue);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_namespace);

    // not a word keyword - `::` cannot collide with an identifier, so it stays a plain string
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_namespace_sep);

    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_ptr);

    // keywords for `ptr`'s reason: `weak` names a type constructor, so it has to be recognised in type
    // position where no identifier can be, and `strong` is its inverse operation. FNC_KEYWORD rather
    // than FNC_STRING, which matches a *prefix* and would break every identifier starting with them
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_weak);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_strong);

    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_null);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_struct);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_class);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_interface);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_enum);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_extern);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_as);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_destructor);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_instanceof);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_mv);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_unsafe);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_private);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_internal);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_public);
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_operator);

    // a keyword even in a build that will drop every test block, because the token filter that drops one
    // has to recognise it - and a word that lexes differently depending on a flag is a word whose meaning
    // no reader can settle
    ECHO_LEX_FNC_KEYWORD(lx_functions, Token::Type::t_test);

    lx_functions.push_back(std::make_unique<LexerFunction::NumericLiteral>());
    lx_functions.push_back(std::make_unique<LexerFunction::StringLiteral>());
    lx_functions.push_back(std::make_unique<LexerFunction::VariableName>());
    lx_functions.push_back(std::make_unique<LexerFunction::HexLiteral>());
    lx_functions.push_back(std::make_unique<LexerFunction::SingleLineComment>());
    lx_functions.push_back(std::make_unique<LexerFunction::MultiLineComment>());
    lx_functions.push_back(std::make_unique<LexerFunction::Identifier>());
    lx_functions.push_back(std::make_unique<LexerFunction::ReferenceFrom>());

    LexerEngine(lx_functions).run(tokens, cursor);
}

void LexerCursor::reset()
 {
    line = 1;
    char_offset = 1;
    it = input.begin();
    determine_end_of_line();
}

std::string LexerCursor::get_code_sample(const std::string::const_iterator it, const uint32_t start_offset, const uint32_t end_offset) const
{
    auto start = it - start_offset;
    auto end = it + end_offset;

    if (start < input.begin()) {
        start = input.begin();
    }

    if (end > input.end()) {
        end = input.end();
    }

    return std::string(start, end);
}

/**
 * Lexer Function Implementations
 *
 * ----------------------------------------------------------------------------
 */


// --- CharToken ---
// ----------------------------------------------------------------------------
const std::vector<std::string> LexerFunction::CharToken::must_match() const
{
    return { std::string(1, lit) };
}

bool LexerFunction::CharToken::parse(TokenCollection &tokens, LexerCursor &cursor) const
{
    if (cursor.peek() != lit) {
        return false;
    }

    tokens.push(std::string(1, lit), type, cursor.line, cursor.char_offset);
    cursor.skip();
    return true;
}

// --- StringToken ---
// ----------------------------------------------------------------------------
const std::vector<std::string> LexerFunction::StringToken::must_match() const
{
    return { lit };
}

bool LexerFunction::StringToken::parse(TokenCollection &tokens, LexerCursor &cursor) const
{
    if (!cursor.begins_with(lit)) {
        return false;
    }

    tokens.push(lit, type, cursor.line, cursor.char_offset);
    cursor.skip(lit.size());
    return true;
}

// --- KeywordToken ---
// ----------------------------------------------------------------------------
bool LexerFunction::KeywordToken::parse(TokenCollection &tokens, LexerCursor &cursor) const
{
    if (!cursor.begins_with(lit)) {
        return false;
    }

    // a keyword is a whole word, so the character that follows it must not be one an identifier
    // could continue with. without this the match is a prefix match and eats the head of every
    // identifier that happens to start with a keyword - `constructor` lexed as `const` +
    // `ructor`, `forward` as `for` + `ward`. peek() answers '\0' past the end, which
    // is not a varname character, so a keyword at EOF still matches
    if (varname_lut[static_cast<unsigned char>(cursor.peek(lit.size()))]) {
        return false;
    }

    // the boundary check is the only thing a keyword adds; how the token itself is emitted stays
    // with the base
    return StringToken::parse(tokens, cursor);
}

// --- NumericLiteral ---
// ----------------------------------------------------------------------------
const std::vector<std::string> LexerFunction::NumericLiteral::must_match() const
{
    return { "-", "" };
}

bool LexerFunction::NumericLiteral::parse(TokenCollection &tokens, LexerCursor &cursor) const
{
    const auto start_offset = cursor.char_offset;
    const auto start_line = cursor.line;

    bool is_negative = cursor.peek() == '-';
    size_t peek_offset = is_negative ? 1 : 0;
    std::string value;

    // no number found, return false
    if (!numeric_lut[cursor.peek(peek_offset)]) {
        return false;
    }

    // skip the negative sign
    if (is_negative) {
        value += '-';
        cursor.skip();
    }

    while (!cursor.is_eof()) {
        if (!numeric_lut[cursor.peek()]) {
            break;
        }
        value += cursor.peek();
        cursor.skip();
    }

    // if the next character is not a dot we have an integer
    //
    // **a dot followed by another dot is not a decimal point.** this reader is a root function, so it
    // runs before the trie is consulted for the character it stops on - and consuming the first dot of
    // a `..` unconditionally is what made `0..10` lex as the float `0.0`, then a stray `.`, then `10`,
    // with no `..` ever formed for the operator registry to match. one character of lookahead is the
    // whole fix, and it costs nothing anywhere else: a decimal point is never followed by a second dot,
    // and `1.` keeps its implicit zero below
    if (cursor.peek() != '.' || cursor.peek(1) == '.') {
        tokens.push(value, Token::Type::t_integer_literal, start_line, start_offset);
        return true;
    }

    // skip the dot
    value += '.';
    cursor.skip();

    // we have a floating point number, so we need to find the fractional part
    while (!cursor.is_eof()) {
        if (!numeric_lut[cursor.peek()]) {
            break;
        }

        value += cursor.peek();
        cursor.skip();
    }

    // if the last character is still a dot, we implicitly add a zero
    if (value.back() == '.') {
        value += '0';
    }

    // our literal support a "f" suffix for float literals instead of double
    if (cursor.peek() == 'f') {
        value += 'f';
        cursor.skip();
    }

    tokens.push(value, Token::Type::t_floating_literal, start_line, start_offset);
    return true;
}

// --- StringLiteral ---
// ----------------------------------------------------------------------------
const std::vector<std::string> LexerFunction::StringLiteral::must_match() const
{
    return { "\"", "'" };
}

bool LexerFunction::StringLiteral::holds_a_hole(const LexerCursor &cursor, char quote) const
{
    // from the opening quote, under the same escape rule the verbatim scan uses - so `"a\"{$x}"`
    // finds the hole and `"a" . "b"` does not go looking past the first literal's end
    auto it = cursor.current() + 1;
    const auto end = cursor.end();

    while (it != end && *it != quote) {
        if (*it == '\\') {
            it++;
            if (it == end) {
                break;
            }
        }
        else if (*it == '{' && (it + 1) != end && *(it + 1) == '$') {
            return true;
        }

        it++;
    }

    return false;
}

void LexerFunction::StringLiteral::lex_verbatim(TokenCollection &tokens, LexerCursor &cursor, char quote) const
{
    const auto string_start_offset = cursor.char_offset;
    const auto string_start_line = cursor.line;

    const auto start = cursor.current();
    cursor.skip();

    while (true) {
        if (cursor.is_eof()) {
            const auto sample = cursor.get_code_sample(start, 20);
            throw Lexer::UnterminatedStringException(sample, string_start_line, string_start_offset);
        }

        if (cursor.peek() == quote) {
            cursor.skip();
            break;
        }

        if (cursor.peek() == '\\') {
            cursor.skip();
        }

        cursor.skip();
    }

    tokens.push(std::string(start, cursor.current()), Token::Type::t_string_literal, string_start_line, string_start_offset);
}

void LexerFunction::StringLiteral::lex_hole(TokenCollection &tokens, LexerCursor &cursor) const
{
    const auto hole_start = cursor.current();
    const auto hole_line = cursor.line;
    const auto hole_offset = cursor.char_offset;

    // braces the hole's own expression opened, so the `}` that closes the hole is the one found at
    // depth zero. counted off the *tokens* rather than the characters, which is what keeps a brace
    // inside a nested string literal from being counted at all
    size_t depth = 0;

    while (true) {
        cursor.skip_formatting();

        if (cursor.is_eof()) {
            const auto sample = cursor.get_code_sample(hole_start, 20);
            throw Lexer::UnterminatedInterpolationException(sample, hole_line, hole_offset);
        }

        const char c = cursor.peek();

        if (c == '}' && depth == 0) {
            cursor.skip();
            return;
        }

        // **the format spec, and the reason the split is decided here rather than on the raw text.**
        // `::` and `:$` are their own tokens and neither introduces a spec, so two characters of
        // look-ahead separate `{$std::io::stdout->fd}` and `{$p:$}` from `{$x:>8}`. what follows is
        // taken raw - `.2f` is not Echo and must never be lexed as any
        if (c == ':' && depth == 0 && cursor.peek(1) != ':' && cursor.peek(1) != '$') {
            cursor.skip();

            const auto spec_line = cursor.line;
            const auto spec_offset = cursor.char_offset;
            std::string spec;

            while (!cursor.is_eof() && cursor.peek() != '}') {
                spec += cursor.peek();
                cursor.skip();
            }

            if (cursor.is_eof()) {
                const auto sample = cursor.get_code_sample(hole_start, 20);
                throw Lexer::UnterminatedInterpolationException(sample, hole_line, hole_offset);
            }

            cursor.skip();
            tokens.push(spec, Token::Type::t_string_interp_spec, spec_line, spec_offset);
            return;
        }

        const size_t before = tokens.size();

        if (!_engine->step(tokens, cursor)) {
            const auto sample = cursor.get_code_sample(cursor.current(), 20);
            throw Lexer::UnknownTokenException(sample, cursor.line, cursor.char_offset);
        }

        for (size_t index = before; index < tokens.size(); index++) {
            const Token::Type type = tokens.tokens[index].type;

            if (type == Token::Type::t_open_brace) {
                depth++;
            }
            else if (type == Token::Type::t_close_brace && depth > 0) {
                depth--;
            }
        }
    }
}

bool LexerFunction::StringLiteral::parse(TokenCollection &tokens, LexerCursor &cursor) const
{
    if (!cursor.is_quote()) {
        return false;
    }

    const auto quote = cursor.peek();

    // **a `'` string never interpolates and a `"` string without a `{$` never grows a second
    // token.** the first half is the language rule; the second is what makes this change invisible
    // to every program written before it - and `_engine` being unset only happens in a lexer built
    // by hand, where the verbatim shape is the only one that can be produced anyway
    if (quote == MHP_VOCAB_SNGQUOTE || _engine == nullptr || !holds_a_hole(cursor, quote)) {
        lex_verbatim(tokens, cursor, quote);
        return true;
    }

    const auto string_start_offset = cursor.char_offset;
    const auto string_start_line = cursor.line;
    const auto start = cursor.current();

    cursor.skip();

    std::string chunk;
    Token::Type chunk_type = Token::Type::t_string_interp_begin;
    size_t chunk_line = string_start_line;
    size_t chunk_offset = string_start_offset;

    while (true) {
        if (cursor.is_eof()) {
            const auto sample = cursor.get_code_sample(start, 20);
            throw Lexer::UnterminatedStringException(sample, string_start_line, string_start_offset);
        }

        if (cursor.peek() == quote) {
            cursor.skip();
            break;
        }

        // only `{$` opens one, so a lone brace is text and needs no escape
        if (cursor.peek() == '{' && cursor.peek(1) == '$') {
            tokens.push(chunk, chunk_type, chunk_line, chunk_offset);
            chunk.clear();

            cursor.skip();
            lex_hole(tokens, cursor);

            chunk_type = Token::Type::t_string_interp_middle;
            chunk_line = cursor.line;
            chunk_offset = cursor.char_offset;
            continue;
        }

        // the escape is carried through undecoded, exactly as the verbatim shape carries it -
        // AST::decode_string_chunk is the one place a `\n` becomes a newline
        if (cursor.peek() == '\\') {
            chunk += cursor.peek();
            cursor.skip();

            if (cursor.is_eof()) {
                const auto sample = cursor.get_code_sample(start, 20);
                throw Lexer::UnterminatedStringException(sample, string_start_line, string_start_offset);
            }
        }

        chunk += cursor.peek();
        cursor.skip();
    }

    tokens.push(chunk, Token::Type::t_string_interp_end, chunk_line, chunk_offset);
    return true;
}

// --- VariableName ---
// ----------------------------------------------------------------------------
const std::vector<std::string> LexerFunction::VariableName::must_match() const
{
    return { "$" };
}

bool LexerFunction::VariableName::parse(TokenCollection &tokens, LexerCursor &cursor) const
{
    if (cursor.peek() != '$') {
        return false;
    }

    auto start_col = cursor.char_offset;
    auto start = cursor.current();
    cursor.skip();

    while (!cursor.is_eof() && varname_lut[cursor.peek()]) {
        cursor.skip();
    }

    tokens.push(std::string(start, cursor.current()), Token::Type::t_varname, cursor.line, start_col);
    return true;
}

// --- HexLiteral ---
// ----------------------------------------------------------------------------
const std::vector<std::string> LexerFunction::HexLiteral::must_match() const
{
    return { "0x", "0X" };
}

bool LexerFunction::HexLiteral::parse(TokenCollection &tokens, LexerCursor &cursor) const
{
    auto start_offset = cursor.char_offset;

    cursor.skip(2);
    std::string value = "0x";

    while (hex_lut[cursor.peek()]) {
        value += cursor.peek();
        cursor.skip();
    }

    tokens.push(value, Token::Type::t_hex_literal, cursor.line, start_offset);

    return true;
}

// --- ReferenceFrom ---
// ----------------------------------------------------------------------------
const std::vector<std::string> LexerFunction::ReferenceFrom::must_match() const
{
    return { "&" };
}

bool LexerFunction::ReferenceFrom::parse(TokenCollection &tokens, LexerCursor &cursor) const
{
    if (cursor.peek() != '&') {
        return false;
    }

    // its only a reference if immediately followed by a var name or identifier
    if (!varname_lut[cursor.peek(1)] && !cursor.begins_with("&$")) {
        return false;
    }

    tokens.push("&", Token::Type::t_ref, cursor.line, cursor.char_offset);
    cursor.skip();
    return true;
}

// --- SingleLineComment ---
// ----------------------------------------------------------------------------
const std::vector<std::string> LexerFunction::SingleLineComment::must_match() const
{
    return { "//" };
}

bool LexerFunction::SingleLineComment::parse(TokenCollection &tokens, LexerCursor &cursor) const
{
    if (!cursor.begins_with("//")) {
        return false;
    }

    cursor.skip(2);
    cursor.skip_until_nl();

    return true;
}

// --- MultiLineComment ---
// ----------------------------------------------------------------------------
const std::vector<std::string> LexerFunction::MultiLineComment::must_match() const
{
    return { "/*" };
}

bool LexerFunction::MultiLineComment::parse(TokenCollection &tokens, LexerCursor &cursor) const
{
    if (!cursor.begins_with("/*")) {
        return false;
    }

    const auto beginning = cursor.current();

    cursor.skip(2);

    while (!cursor.is_eof()) {
        if (cursor.begins_with("*/")) {
            cursor.skip(2);
            return true;
        }

        cursor.skip();
    }

    // get some context around the unterminated comment
    auto extract = std::string(beginning, std::min(cursor.current() + 20, cursor.input.end()));
    throw Lexer::UnterminatedCommentException(extract, cursor.line, cursor.char_offset);
}

// --- Identifier ---
// ----------------------------------------------------------------------------
const std::vector<std::string> LexerFunction::Identifier::must_match() const
{
    return { "" };
}

bool LexerFunction::Identifier::parse(TokenCollection &tokens, LexerCursor &cursor) const
{
    auto start_offset = cursor.char_offset;
    auto start_line = cursor.line;

    const auto start = cursor.current();

    while (!cursor.is_eof()) {
        if (!varname_lut[cursor.peek()]) {
            break;
        }
        cursor.skip();
    }

    // sanity check
    if (start == cursor.current()) {
        return false;
    }

    tokens.push(std::string(start, cursor.current()), Token::Type::t_identifier, start_line, start_offset);
    return true;
}
