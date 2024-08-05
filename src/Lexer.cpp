#include "Lexer.h"

#include <algorithm>
#include <cassert>

constexpr bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') || 
           (c >= 'a' && c <= 'f') || 
           (c >= 'A' && c <= 'F');
}

constexpr bool is_numeric_char(char c) {
    return (c >= '0' && c <= '9');
}

constexpr bool is_valid_varname_char(char c) {
    return (c >= 'a' && c <= 'z') || 
           (c >= 'A' && c <= 'Z') || 
           (c >= '0' && c <= '9') || 
           (c == '_');
}

constexpr bool is_seperating_char(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '(' || c == ')' || c == '{' || c == '}' ||
           c == '[' || c == ']' || c == ';' || c == ',';
}

constexpr std::array<bool, 256> generate_hex_lut() {
    std::array<bool, 256> table = {};
    for (size_t i = 0; i < 256; ++i) {
        table[i] = is_hex_char(static_cast<char>(i));
    }
    return table;
}

constexpr std::array<bool, 256> generate_numeric_lut() {
    std::array<bool, 256> table = {};
    for (size_t i = 0; i < 256; ++i) {
        table[i] = is_numeric_char(static_cast<char>(i));
    }
    return table;
}

constexpr std::array<bool, 256> generate_varname_lut() {
    std::array<bool, 256> table = {};
    for (size_t i = 0; i < 256; ++i) {
        table[i] = is_valid_varname_char(static_cast<char>(i));
    }
    return table;
}

constexpr std::array<bool, 256> generate_seperating_lut() {
    std::array<bool, 256> table = {};
    for (size_t i = 0; i < 256; ++i) {
        table[i] = is_seperating_char(static_cast<char>(i));
    }
    return table;
}

constexpr auto hex_lut = generate_hex_lut();
constexpr auto numeric_lut = generate_numeric_lut();
constexpr auto varname_lut = generate_varname_lut();
constexpr auto seperating_lut = generate_seperating_lut();
 
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

void Lexer::execute_functions(FunctionList &functions, TokenCollection &tokens, LexerCursor &cursor)
{
    // we build a tree like structure based on the "must_match" string of each function
    // we simply take the first N chars of the must match 
    // im sure there is a better way to do this but works for now
    auto fnc_tree_root = std::make_unique<LexerFunction::TreeNode>("root");

    size_t prefix_limit = 3;
    for (auto &func : functions) {
        auto machter_stringns = func->must_match();
        for (auto &match : machter_stringns) {
            insert_function_into_tree(*fnc_tree_root.get(), match, func.get(), prefix_limit);
        }
    }

    sort_functiontree(*fnc_tree_root.get());

    while (!cursor.is_eof()) 
    {
        // formatting aka whitespace, tabs, newlines
        if (cursor.is_formatting()) {
            cursor.skip_formatting();
            if (cursor.is_eof()) {
                break;
            }
        }

        auto node = fnc_tree_root.get();
        auto peek_offset = 0;
        while (!cursor.is_eof() && node->children.find(cursor.peek(peek_offset)) != node->children.end()) {
            node = node->children[cursor.peek(peek_offset)].get();
            peek_offset++;
        }

        bool matched = false;

        if (node->functions.empty()) {
            // run root functions
            for (auto &func : fnc_tree_root->functions) {
                if (func->parse(tokens, cursor)) {
                    matched = true;
                    break;
                }
            }
        }

        for (auto &func : node->functions) {
            if (func->parse(tokens, cursor)) {
                matched = true;
                break;
            }
        }

        // if still nothing matched retry with the root functions
        if (!matched) {
            for (auto &func : fnc_tree_root->functions) {
                if (func->parse(tokens, cursor)) {
                    matched = true;
                    break;
                }
            }
        }

        if (!matched) {
            throw UnknownTokenException("Unexpected", cursor.line, cursor.char_offset );
        }
    }
}

// bool Lexer::parse_varname(TokenCollection &tokens, LexerCursor &cursor)
// {
//     // just use a regex for now
//     if (cursor.peek() != '$') {
//         return false;
//     }
    
//     auto start_col = cursor.char_offset;
//     auto start = cursor.it;
//     cursor.skip();

//     while (!cursor.is_eof() && varname_lut[cursor.peek()]) {
//         cursor.skip();
//     }

//     tokens.push(std::string(start, cursor.it), Token::Type::t_varname, cursor.line, start_col);

//     return true;
// }

// bool Lexer::parse_string_literal(TokenCollection &tokens, LexerCursor &cursor) 
// {
//     if (!cursor.is_quote()) {
//         return false;
//     }

//     const auto string_start_offset = cursor.char_offset;
//     const auto string_start_line = cursor.line;

//     auto start = cursor.it;
//     auto quote = cursor.peek();
//     cursor.skip();

//     while (true) {
//         if (cursor.is_eof()) {
//             throw UnterminatedStringException { std::string(start, cursor.it), string_start_line, string_start_offset };
//         }

//         if (cursor.peek() == quote) {
//             cursor.skip();
//             break;
//         }

//         if (cursor.peek() == '\\') {
//             cursor.skip();
//         }

//         cursor.skip();
//     }

//     tokens.push(std::string(start, cursor.it), Token::Type::t_string_literal, string_start_line, string_start_offset);

//     return true;
// }

// bool Lexer::parse_hex_literal(TokenCollection &tokens, LexerCursor &cursor) {

//     if (!cursor.begins_with("0x")) {
//         return false;
//     }

//     cursor.skip(2);
//     std::string value = "0x";

//     while (hex_lut[cursor.peek()]) {
//         value += cursor.peek();
//         cursor.skip();
//     }

//     tokens.push(value, Token::Type::t_hex_literal, cursor.line, cursor.char_offset);

//     return true;
// }

// bool Lexer::parse_sl_comment(TokenCollection &tokens, LexerCursor &cursor)
// {
//     if (!cursor.begins_with("//")) {
//         return false;
//     }

//     cursor.skip(2);
//     cursor.skip_until_nl();

//     return true;
// }

// bool Lexer::parse_ml_comment(TokenCollection &tokens, LexerCursor &cursor)
// {
//     return false;
// }

#define ECHO_LEX_MAKE_FNCLIST(name) \
    std::vector<std::unique_ptr<LexerFunction::Base>> name;

#define ECHO_LEX_FNC_CHAR(name, type) \
    assert(token_lit_symbol_string(type).length() == 1 && "Char token must have a length of 1"); \
    name.push_back(std::make_unique<LexerFunction::CharToken>(token_lit_symbol_string(type).at(0), type));

#define ECHO_LEX_FNC_STRING(name, type) \
    name.push_back(std::make_unique<LexerFunction::StringToken>(token_lit_symbol_string(type), type));

#define ECHO_LEX_FNC_CUST_STRING(name, lit, type) \
    name.push_back(std::make_unique<LexerFunction::StringToken>(lit, type));

void Lexer::tokenize(TokenCollection &tokens, const std::string &input, const AST::OperatorRegistry *op_registry) 
{   
    auto cursor = LexerCursor(input);

    // build a list of lexer functions
    ECHO_LEX_MAKE_FNCLIST(lx_functions);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_semicolon);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_colon);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_comma);
    ECHO_LEX_FNC_CHAR(lx_functions, Token::Type::t_dot);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_logical_and);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_logical_or);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_logical_eq);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_logical_neq);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_logical_leq);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_logical_geq);
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
    ECHO_LEX_FNC_CUST_STRING(lx_functions, "true", Token::Type::t_bool_literal);
    ECHO_LEX_FNC_CUST_STRING(lx_functions, "false", Token::Type::t_bool_literal);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_const);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_echo);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_function);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_return);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_if);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_else);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_while);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_for);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_break);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_continue);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_namespace);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_namespace_sep);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_ptr);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_struct);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_class);
    ECHO_LEX_FNC_STRING(lx_functions, Token::Type::t_enum);

    lx_functions.push_back(std::make_unique<LexerFunction::NumericLiteral>());
    lx_functions.push_back(std::make_unique<LexerFunction::StringLiteral>());
    lx_functions.push_back(std::make_unique<LexerFunction::VariableName>());
    lx_functions.push_back(std::make_unique<LexerFunction::HexLiteral>());
    lx_functions.push_back(std::make_unique<LexerFunction::SingleLineComment>());
    lx_functions.push_back(std::make_unique<LexerFunction::MultiLineComment>());
    lx_functions.push_back(std::make_unique<LexerFunction::Identifier>());
    lx_functions.push_back(std::make_unique<LexerFunction::ReferenceFrom>());

    // if there are some custom operators 
    // we add them to the list of functions so that they are recognized
    if (op_registry) {
        for (const auto op : op_registry->get_custom_operators()) {
            ECHO_LEX_FNC_CUST_STRING(lx_functions, op->name, Token::Type::t_op_custom);
        }
    }

    execute_functions(lx_functions, tokens, cursor);

    // recreate cursor for mig
    // cursor.reset();

    // std::vector<Lexer::LexerFunctionSignature> functions = {
    //     &Lexer::parse_string_literal,
    //     &Lexer::parse_varname,
    //     &Lexer::parse_sl_comment,
    //     &Lexer::parse_ml_comment,
    //     &Lexer::parse_char_token<';', Token::Type::t_semicolon>,
    //     &Lexer::parse_char_token<':', Token::Type::t_colon>,
    //     &Lexer::parse_char_token<',', Token::Type::t_comma>,
    //     &Lexer::parse_char_token<'.', Token::Type::t_dot>,
    //     &Lexer::parse_exact_token<"&&", Token::Type::t_logical_and>,
    //     &Lexer::parse_exact_token<"||", Token::Type::t_logical_or>,
    //     &Lexer::parse_exact_token<"==", Token::Type::t_logical_eq>,
    //     &Lexer::parse_exact_token<"!=", Token::Type::t_logical_neq>,
    //     &Lexer::parse_exact_token<"<=", Token::Type::t_logical_leq>,
    //     &Lexer::parse_exact_token<">=", Token::Type::t_logical_geq>,
    //     &Lexer::parse_char_token<'=', Token::Type::t_assign>,
    //     &Lexer::parse_exact_token<"++", Token::Type::t_op_inc>,
    //     &Lexer::parse_exact_token<"--", Token::Type::t_op_dec>,
    //     &Lexer::parse_char_token<'+', Token::Type::t_op_add>,
    //     &Lexer::parse_char_token<'*', Token::Type::t_op_mul>,
    //     &Lexer::parse_char_token<'/', Token::Type::t_op_div>,
    //     &Lexer::parse_char_token<'%', Token::Type::t_op_mod>,
    //     &Lexer::parse_char_token<'^', Token::Type::t_op_pow>,
    //     &Lexer::parse_char_token<'?', Token::Type::t_qmark>,
    //     &Lexer::parse_char_token<'!', Token::Type::t_exclamation>,
    //     &Lexer::parse_char_token<'<', Token::Type::t_open_angle>,
    //     &Lexer::parse_char_token<'>', Token::Type::t_close_angle>,
    //     &Lexer::parse_char_token<'(', Token::Type::t_open_paren>,
    //     &Lexer::parse_char_token<')', Token::Type::t_close_paren>,
    //     &Lexer::parse_char_token<'{', Token::Type::t_open_brace>,
    //     &Lexer::parse_char_token<'}', Token::Type::t_close_brace>,
    //     &Lexer::parse_char_token<'[', Token::Type::t_open_bracket>,
    //     &Lexer::parse_char_token<']', Token::Type::t_close_bracket>,
    //     &Lexer::parse_exact_token<"true", Token::Type::t_bool_literal>,
    //     &Lexer::parse_exact_token<"false", Token::Type::t_bool_literal>,
    //     &Lexer::parse_exact_token<"const", Token::Type::t_const>,
    //     &Lexer::parse_exact_token<"echo", Token::Type::t_echo>,
    //     &Lexer::parse_hex_literal,
    //     &Lexer::parse_regex_token<"^-?[0-9]+\\.[0-9]+f?", Token::Type::t_floating_literal>,
    //     &Lexer::parse_regex_token<"^-?[0-9]+", Token::Type::t_integer_literal>,
    //     &Lexer::parse_char_token<'-', Token::Type::t_op_sub>,
    //     // generic identifier
    //     &Lexer::parse_regex_token<"^[_a-zA-Z0-9]+", Token::Type::t_identifier>
    // };

    // while (!cursor.is_eof()) 
    // {
    //     // formatting aka whitespace, tabs, newlines
    //     if (cursor.is_formatting()) {
    //         cursor.skip_formatting();
    //         if (cursor.is_eof()) {
    //             break;
    //         }
    //     }

    //     bool matched = false;

    //     for (auto &func : functions) {
    //         if (func(*this, tokens, cursor)) {
    //             matched = true;
    //             break;
    //         }
    //     }

    //     if (!matched) {
    //         // construct a string with some context around the unknown token
    //         if (cursor.it + 10 < cursor.input.end()) {
    //             throw UnknownTokenException { std::string(cursor.it, cursor.it + 10), cursor.line, cursor.char_offset };
    //         } else {
    //             auto begin = cursor.it - 10;
    //             if (begin < cursor.input.begin()) {
    //                 begin = cursor.input.begin();
    //             }
    //             auto end = cursor.it + 10;
    //             if (end > cursor.input.end()) {
    //                 end = cursor.input.end();
    //             }
    //             auto extract = std::string(begin, end);

    //             throw UnknownTokenException { extract, cursor.line, cursor.char_offset };
    //         }
    //     }
    // }
}

void Lexer::tokenize_prepass_operators(const std::string &input, AST::OperatorRegistry &op_registry)
{
    // in this prepass we really only care to find custom operators in the input
    // so we can register them and let the main tokenizer handle the rest
    auto cursor = LexerCursor(input);

    while (!cursor.is_eof()) 
    {
        // formatting aka whitespace, tabs, newlines
        cursor.skip_formatting();

        // nothing left to parse = done
        if (cursor.is_eof()) {
            break;
        }

        // operator defintions always begin with an "operator" keyword
        // the operator keyword must be the first non-formatting character on the line
        if (!cursor.begins_with("operator")) {
            cursor.skip_until_nl();
            continue;
        }

        cursor.skip(8); // skip "operator"

        // after the operator there might be some precedence defition
        //   operator(100, left)
        // we still do not know if its a unary or binary operator but in the prepass we
        // really only care to find the operator symbol / name
        cursor.skip_formatting();
        cursor.skip_scope('(', ')');

        // now we would be here for a binary op
        //   (int $a) ++ (int $b) : int
        // unary op with prefix
        //   ++(int $a) : int
        // unary op with postfix
        //   (int $a)++ : int
        // or 
        //   ++ : int

        // considering that we can simply do this again
        cursor.skip_formatting();
        cursor.skip_scope('(', ')');
        cursor.skip_formatting();

        // and now we got
        //   ++ (int $a) : int
        //   ++(int $a) : int
        //   ++ : int
        //   ++ : int
        // so in all cases we are at the beginning of the operator symbol
        const auto start = cursor.current();

        // skip until the next whitespace or newline
        while(!cursor.is_eof() && !cursor.is_formatting() && !cursor.is_seperating_char()) {
            cursor.skip();
        }

        // now we have the operator symbol
        auto op = std::string(start, cursor.current());

        // check if the operator is already registered
        if (!op_registry.get_operator(op)) {
            op_registry.register_custom_op(op, -1, AST::OpAssociativity::left);
        }

        // skip until the end of the line
        cursor.skip_until_nl();
    }
}

void LexerCursor::reset()
 {
    line = 1;
    char_offset = 1;
    it = input.begin();
    determine_end_of_line();
}

void LexerCursor::skip_scope(const char open, const char close)
{
    int depth = 0;

    // not at the start of a scope
    if (peek() != open) {
        return;
    }

    while (!is_eof()) {
        if (peek() == open) {
            depth++;
        } else if (peek() == close) {
            depth--;
            if (depth == 0) {
                skip();
                return;
            }
        }
        skip();
    }
}

bool LexerCursor::is_seperating_char(size_t offset)
{
    if (offset >= input.size()) {
        return true;
    }
    return seperating_lut[peek(offset)];
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
    if (cursor.peek() != '.') {
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

bool LexerFunction::StringLiteral::parse(TokenCollection &tokens, LexerCursor &cursor) const
{
    if (!cursor.is_quote()) {
        return false;
    }

    const auto string_start_offset = cursor.char_offset;
    const auto string_start_line = cursor.line;

    const auto start = cursor.current();
    const auto quote = cursor.peek();
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
