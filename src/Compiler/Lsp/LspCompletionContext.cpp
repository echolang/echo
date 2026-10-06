#include "Compiler/Lsp/LspCompletionContext.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace
{
    using Compiler::Lsp::CompletionContext;
    using Compiler::Lsp::CompletionKind;
    using Compiler::Lsp::FrameKind;
    using Compiler::Lsp::LexKind;
    using Compiler::Lsp::LexToken;
    using Compiler::Lsp::LexicalFrame;
    using Compiler::Lsp::LexicalVariable;
    using Compiler::Lsp::NamePosition;
    using Compiler::Lsp::ReceiverSegment;

    // longest first, so `?->` is one token
    constexpr std::string_view k_punctuation[] = {
        "?->", "...", "..=", "->", "::", "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "++", "--",
        "+=", "-=", "*=", "/=", "%=", "..", "<<", ">>", "#[" };

    // the words that cannot be the last word of a type. what decides whether `x $name` declares
    // `$name` with type `x`, or is `return $name` / `echo $name` / `as $name`
    const std::unordered_set<std::string> &non_type_words()
    {
        static const std::unordered_set<std::string> words = {
            "as", "return", "echo", "if", "else", "while", "for", "foreach", "match", "guard", "mv",
            "instanceof", "case", "use", "namespace", "function", "struct", "class", "enum", "interface",
            "test", "const", "public", "private", "internal", "static", "unsafe", "break", "continue",
            "true", "false", "null", "strong", "weak", "operator", "extern", "destructor", "constructor",
            "die" };
        return words;
    }

    bool is_ident_char(char c)
    {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    bool is_ident_start(char c)
    {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }

    bool is_punct(const LexToken *token, std::string_view text)
    {
        return token != nullptr && token->kind == LexKind::t_punct && token->text == text;
    }

    bool is_word(const LexToken *token, std::string_view text)
    {
        return token != nullptr && token->kind == LexKind::t_identifier && token->text == text;
    }

    const LexToken *at(const std::vector<LexToken> &tokens, long index)
    {
        if (index < 0 || static_cast<size_t>(index) >= tokens.size()) {
            return nullptr;
        }

        return &tokens[static_cast<size_t>(index)];
    }

    // can this token end a type: `int32`, `Point`, `array<int32>`, `T?`, `int32[]`, `T&`
    bool ends_type(const LexToken *token)
    {
        if (token == nullptr) {
            return false;
        }

        if (token->kind == LexKind::t_identifier) {
            return non_type_words().count(token->text) == 0;
        }

        if (token->kind != LexKind::t_punct) {
            return false;
        }

        return token->text == ">" || token->text == ">>" || token->text == "?" || token->text == "]"
            || token->text == "&" || token->text == "*";
    }

    // the written type ending at `end`, read backwards: `array<rc<Point>>`, `geo::Point?`. stops at
    // anything that cannot be part of one, and at a second identifier with no `::` between: the
    // `public` of `public int32 $x`, the label of `forEvent: string $name`
    std::string type_text_ending_at(const std::vector<LexToken> &tokens, long end)
    {
        int angles = 0;
        long index = end;
        bool want_name = true;

        while (index >= 0) {
            const LexToken &token = tokens[static_cast<size_t>(index)];

            if (token.kind == LexKind::t_identifier) {
                if (!want_name || (angles == 0 && non_type_words().count(token.text) > 0)) {
                    break;
                }

                want_name = false;
                index--;
                continue;
            }

            if (token.kind != LexKind::t_punct) {
                break;
            }

            const std::string &p = token.text;
            if (p == ">") {
                angles++;
            }
            else if (p == ">>") {
                angles += 2;
            }
            else if (p == "<") {
                if (angles == 0) {
                    break;
                }
                angles--;
            }
            else if (p == "," && angles == 0) {
                break;
            }
            else if (p != "::" && p != "," && p != "?" && p != "&" && p != "*" && p != "[" && p != "]") {
                break;
            }

            // after a `::` or inside `<...>` a name is wanted again; after a suffix like `?` the
            // name it decorates still is
            want_name = true;
            index--;
        }

        if (angles != 0) {
            return "";
        }

        std::string out;
        for (long i = index + 1; i <= end; i++) {
            out += tokens[static_cast<size_t>(i)].text;
        }

        return out;
    }

    // what follows the `=` at `equals`, to the end of the statement, a body brace, or 32 tokens
    std::vector<LexToken> initializer_after(const std::vector<LexToken> &tokens, size_t equals)
    {
        std::vector<LexToken> out;
        int depth = 0;
        for (size_t i = equals + 1; i < tokens.size() && out.size() < 32; i++) {
            const LexToken &token = tokens[i];
            if (token.kind == LexKind::t_punct) {
                if (token.text == "(" || token.text == "[") {
                    depth++;
                }
                else if (token.text == ")" || token.text == "]") {
                    if (depth == 0) {
                        break;
                    }
                    depth--;
                }
                else if ((token.text == ";" && depth == 0) || token.text == "{" || token.text == "}") {
                    break;
                }
            }

            out.push_back(token);
        }

        return out;
    }

    // the `a::b::c` ending at `end` (an identifier), read backwards. `start_out` is where it starts
    std::vector<std::string> path_ending_at(const std::vector<LexToken> &tokens, long end, long &start_out)
    {
        std::vector<std::string> path;
        long index = end;
        while (index >= 0 && tokens[static_cast<size_t>(index)].kind == LexKind::t_identifier) {
            path.insert(path.begin(), tokens[static_cast<size_t>(index)].text);
            if (!is_punct(at(tokens, index - 1), "::")) {
                break;
            }
            index -= 2;
        }

        start_out = index;
        return path;
    }

    // the matching opener of the closer at `close`, or -1
    long matching_open(const std::vector<LexToken> &tokens, long close, std::string_view open_text, std::string_view close_text)
    {
        int depth = 0;
        for (long i = close; i >= 0; i--) {
            const LexToken &token = tokens[static_cast<size_t>(i)];
            if (token.kind != LexKind::t_punct) {
                continue;
            }

            if (token.text == close_text) {
                depth++;
            }
            else if (token.text == open_text) {
                depth--;
                if (depth == 0) {
                    return i;
                }
            }
        }

        return -1;
    }

    struct OpenFrame
    {
        LexicalFrame frame;
        int parens = 0;
        size_t statement_begin = 0;

        // `use a::{x, y}`: the braces are an import group
        bool use_group = false;
        std::vector<std::string> use_path;
    };

    // a declaration's header seen before its `{`: a function's parameters, a foreach's binding, a
    // type's name. they belong to the frame the next brace opens
    struct Pending
    {
        bool active = false;
        FrameKind kind = FrameKind::t_block;
        std::vector<LexicalVariable> variables;
        std::string name;
        size_t name_offset = 0;
        bool is_static = false;
        bool saw_as = false;
        bool callable = false;
    };

    bool statement_has_word(const std::vector<LexToken> &tokens, size_t begin, size_t end, std::string_view word)
    {
        for (size_t i = begin; i < end && i < tokens.size(); i++) {
            if (is_word(&tokens[i], word)) {
                return true;
            }
        }

        return false;
    }

    // the frame walk. fills `frames` with what is open at the end of `tokens`, and says what the last
    // frame's current statement and pending header look like, for classification
    struct Walk
    {
        std::vector<OpenFrame> open;
        Pending pending;
    };

    Walk walk_frames(const std::vector<LexToken> &tokens)
    {
        Walk walk;
        walk.open.push_back(OpenFrame{});

        for (size_t i = 0; i < tokens.size(); i++) {
            const LexToken &token = tokens[i];
            OpenFrame &top = walk.open.back();
            Pending &pending = walk.pending;
            const LexToken *prev = at(tokens, static_cast<long>(i) - 1);
            const LexToken *next = at(tokens, static_cast<long>(i) + 1);

            if (token.kind == LexKind::t_identifier) {
                const std::string &word = token.text;

                if (word == "function" && next != nullptr && !is_punct(next, "<")) {
                    pending = Pending{};
                    pending.active = true;
                    pending.callable = true;
                    pending.is_static = statement_has_word(tokens, top.statement_begin, i, "static");
                    if (next->kind == LexKind::t_identifier) {
                        pending.kind = FrameKind::t_function;
                        pending.name = next->text;
                        pending.name_offset = next->offset;
                    }
                    else {
                        pending.kind = FrameKind::t_closure;
                    }
                }
                else if ((word == "constructor" || word == "destructor")
                    && top.frame.kind == FrameKind::t_type && is_punct(next, "(")) {
                    pending = Pending{};
                    pending.active = true;
                    pending.callable = true;
                    pending.kind = FrameKind::t_function;
                }
                else if (word == "init" && top.frame.kind == FrameKind::t_type && is_punct(next, "{")) {
                    pending = Pending{};
                    pending.active = true;
                    pending.kind = FrameKind::t_function;
                }
                else if (word == "test" && i == top.statement_begin && next != nullptr
                    && next->kind == LexKind::t_identifier) {
                    pending = Pending{};
                    pending.active = true;
                    pending.kind = FrameKind::t_function;
                    pending.name = next->text;
                    pending.name_offset = next->offset;
                }
                else if ((word == "struct" || word == "class" || word == "enum" || word == "interface")
                    && next != nullptr && next->kind == LexKind::t_identifier && top.parens == 0) {
                    pending = Pending{};
                    pending.active = true;
                    pending.kind = FrameKind::t_type;
                    pending.name = next->text;
                    pending.name_offset = next->offset;
                }
                else if ((word == "foreach" || word == "for" || word == "if" || word == "while"
                    || word == "match") && !pending.active) {
                    pending = Pending{};
                    pending.active = true;
                    pending.kind = FrameKind::t_block;
                }
                else if (word == "as" && pending.active && top.parens > 0) {
                    pending.saw_as = true;
                }

                continue;
            }

            if (token.kind == LexKind::t_variable) {
                if (token.text == "$" || token.text == "$this") {
                    continue;
                }

                // a header's binding: a parameter, a for's counter, a foreach's `as $v` / `=> $v`
                if (pending.active && top.parens > 0) {
                    const bool typed = ends_type(prev)
                        && (is_punct(next, ",") || is_punct(next, ")") || is_punct(next, "=") || is_punct(next, ";"));
                    const bool bound = pending.saw_as && (is_word(prev, "as") || is_punct(prev, "=>"));
                    if (typed || bound) {
                        LexicalVariable variable;
                        variable.name = token.text;
                        variable.offset = token.offset;
                        variable.is_parameter = pending.callable;
                        if (typed) {
                            variable.written_type = type_text_ending_at(tokens, static_cast<long>(i) - 1);
                        }
                        if (is_punct(next, "=")) {
                            variable.initializer = initializer_after(tokens, i + 1);
                        }
                        pending.variables.push_back(std::move(variable));
                    }

                    continue;
                }

                // a statement's declaration: `Type $x = ...;`, `Type $x;`, `$x = ...;`. a type body's
                // `int32 $x;` is a property, which `$` cannot name
                if (top.parens == 0 && top.frame.kind != FrameKind::t_type) {
                    const bool typed = ends_type(prev) && (is_punct(next, "=") || is_punct(next, ";"));
                    const bool assigned = i == top.statement_begin && is_punct(next, "=");
                    if (typed || assigned) {
                        LexicalVariable variable;
                        variable.name = token.text;
                        variable.offset = token.offset;
                        if (typed) {
                            variable.written_type = type_text_ending_at(tokens, static_cast<long>(i) - 1);
                        }
                        if (is_punct(next, "=")) {
                            variable.initializer = initializer_after(tokens, i + 1);
                        }
                        top.frame.variables.push_back(std::move(variable));
                    }
                }

                continue;
            }

            if (token.kind != LexKind::t_punct) {
                continue;
            }

            if (token.text == "(" || token.text == "[") {
                top.parens++;
            }
            else if (token.text == ")" || token.text == "]") {
                top.parens = std::max(0, top.parens - 1);
            }
            else if (token.text == ";" && top.parens == 0) {
                pending = Pending{};
                top.statement_begin = i + 1;
            }
            else if (token.text == "{") {
                OpenFrame frame;
                frame.frame.brace_offset = token.offset;
                if (pending.active) {
                    frame.frame.kind = pending.kind;
                    frame.frame.variables = std::move(pending.variables);
                    frame.frame.name = pending.name;
                    frame.frame.name_offset = pending.name_offset;
                    frame.frame.is_static = pending.is_static;
                }

                // `use a::b::{`: the statement this brace belongs to began with `use`
                if (is_punct(prev, "::") && top.statement_begin < tokens.size()
                    && is_word(&tokens[top.statement_begin], "use")) {
                    long start = 0;
                    frame.use_group = true;
                    frame.use_path = path_ending_at(tokens, static_cast<long>(i) - 2, start);
                }

                frame.statement_begin = i + 1;
                pending = Pending{};
                walk.open.push_back(std::move(frame));
            }
            else if (token.text == "}") {
                if (walk.open.size() > 1) {
                    walk.open.pop_back();
                }

                pending = Pending{};
                walk.open.back().statement_begin = i + 1;
            }
        }

        return walk;
    }

    bool is_keyword_like(const LexToken &token)
    {
        return token.kind == LexKind::t_identifier && non_type_words().count(token.text) > 0;
    }

    NamePosition position_of(const std::vector<LexToken> &tokens, const Walk &walk)
    {
        const OpenFrame &top = walk.open.back();
        const LexToken *last = tokens.empty() ? nullptr : &tokens.back();
        const LexToken *before = at(tokens, static_cast<long>(tokens.size()) - 2);

        if (last == nullptr || tokens.size() == top.statement_begin || is_word(last, "else")) {
            if (top.frame.kind == FrameKind::t_type) {
                return NamePosition::t_type_body;
            }

            return walk.open.size() == 1 ? NamePosition::t_file_root : NamePosition::t_statement;
        }

        // `) : |` a return type; `as |`, `instanceof |` a cast or test; `<|` a type argument
        if ((is_punct(last, ":") && is_punct(before, ")")) || is_word(last, "as")
            || is_word(last, "instanceof") || is_punct(last, "<")) {
            return NamePosition::t_type;
        }

        // the start of a parameter in a function header
        if (walk.pending.active && walk.pending.callable && top.parens > 0
            && (is_punct(last, "(") || is_punct(last, ","))) {
            return NamePosition::t_type;
        }

        if (last->kind == LexKind::t_variable || last->kind == LexKind::t_number
            || last->kind == LexKind::t_string || is_punct(last, ")") || is_punct(last, "]")
            || (last->kind == LexKind::t_identifier && !is_keyword_like(*last))) {
            return NamePosition::t_after_expression;
        }

        return NamePosition::t_expression;
    }

    void classify_shorthand(CompletionContext &context, const std::vector<LexToken> &tokens)
    {
        const long dot = static_cast<long>(tokens.size()) - 1;
        const LexToken *before = at(tokens, dot - 1);
        if (before == nullptr) {
            return;
        }

        if (is_word(before, "return")) {
            context.kind = CompletionKind::t_shorthand;
            context.destination_is_return = true;
            return;
        }

        if (before->kind != LexKind::t_punct) {
            return;
        }

        const std::string &p = before->text;
        if (p == "=") {
            context.kind = CompletionKind::t_shorthand;
            const LexToken *target = at(tokens, dot - 2);
            if (target != nullptr && target->kind == LexKind::t_variable && ends_type(at(tokens, dot - 3))) {
                context.destination_type = type_text_ending_at(tokens, dot - 3);
                return;
            }

            context.destination_chain = Compiler::Lsp::receiver_chain_of(
                std::vector<LexToken>(tokens.begin(), tokens.begin() + (dot - 1)));
            return;
        }

        if (p == "==" || p == "!=" || p == "??") {
            context.kind = CompletionKind::t_shorthand;
            context.destination_chain = Compiler::Lsp::receiver_chain_of(
                std::vector<LexToken>(tokens.begin(), tokens.begin() + (dot - 1)));
            return;
        }

        // a call argument or a match arm. the destination is real; the text alone cannot name it
        if (p == "(" || p == "," || p == "=>" || p == "{") {
            context.kind = CompletionKind::t_shorthand;
        }
    }
};

std::vector<Compiler::Lsp::LexToken> Compiler::Lsp::lex_echo(std::string_view text, size_t end, bool &cursor_in_code)
{
    const std::string_view src = text.substr(0, std::min(end, text.size()));
    const size_t n = src.size();
    std::vector<LexToken> out;

    // in_string: scanning the inside of a "..." literal. holes: one entry per open `{$ ... }`
    // interpolation hole, counting the braces nested inside it. a string inside a hole and a hole
    // inside that string nest through the same two, because each always resumes the other
    bool in_string = false;
    std::vector<int> holes;
    bool unterminated = false;
    size_t i = 0;

    auto push = [&](LexKind kind, size_t from, size_t to) {
        out.push_back(LexToken{ kind, std::string(src.substr(from, to - from)), from });
    };

    while (i < n) {
        if (in_string) {
            const char c = src[i];
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == '"') {
                in_string = false;
                i++;
                continue;
            }
            // the `$` can be the first character past `end`: `"{$|` is a hole being typed
            if (c == '{' && i + 1 < text.size() && text[i + 1] == '$') {
                holes.push_back(0);
                in_string = false;
                i++;
                continue;
            }
            i++;
            continue;
        }

        const char c = src[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            i++;
            continue;
        }

        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            const size_t newline = src.find('\n', i);
            if (newline == std::string_view::npos) {
                unterminated = true;
                break;
            }
            i = newline + 1;
            continue;
        }

        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            const size_t close = src.find("*/", i + 2);
            if (close == std::string_view::npos) {
                unterminated = true;
                break;
            }
            i = close + 2;
            continue;
        }

        if (c == '"') {
            push(LexKind::t_string, i, i + 1);
            in_string = true;
            i++;
            continue;
        }

        if (c == '\'') {
            size_t j = i + 1;
            while (j < n && src[j] != '\'') {
                j += src[j] == '\\' ? 2 : 1;
            }
            if (j >= n) {
                unterminated = true;
                break;
            }
            push(LexKind::t_string, i, j + 1);
            i = j + 1;
            continue;
        }

        if (!holes.empty() && (c == '{' || c == '}')) {
            if (c == '}' && holes.back() == 0) {
                holes.pop_back();
                in_string = true;
                i++;
                continue;
            }

            holes.back() += c == '{' ? 1 : -1;
        }

        if (c == '$') {
            size_t j = i + 1;
            while (j < n && is_ident_char(src[j])) {
                j++;
            }
            push(LexKind::t_variable, i, j);
            i = j;
            continue;
        }

        if (is_ident_start(c)) {
            size_t j = i + 1;
            while (j < n && is_ident_char(src[j])) {
                j++;
            }
            push(LexKind::t_identifier, i, j);
            i = j;
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i + 1;
            while (j < n && (is_ident_char(src[j])
                || (src[j] == '.' && j + 1 < n && std::isdigit(static_cast<unsigned char>(src[j + 1]))))) {
                j++;
            }
            push(LexKind::t_number, i, j);
            i = j;
            continue;
        }

        size_t width = 1;
        for (std::string_view punct : k_punctuation) {
            if (src.substr(i, punct.size()) == punct) {
                width = punct.size();
                break;
            }
        }

        push(LexKind::t_punct, i, i + width);
        i += width;
    }

    cursor_in_code = !in_string && !unterminated;
    return out;
}

std::vector<Compiler::Lsp::ReceiverSegment> Compiler::Lsp::receiver_chain_of(const std::vector<LexToken> &tokens)
{
    std::vector<ReceiverSegment> chain;
    long index = static_cast<long>(tokens.size()) - 1;

    // the root ends the walk. whatever is in front of it (`guard`, `mv`, an operator) sits outside
    // the chain
    auto finish = [&](ReceiverSegment root) {
        chain.insert(chain.begin(), std::move(root));
        return chain;
    };

    while (index >= 0) {
        const LexToken &token = tokens[static_cast<size_t>(index)];

        if (is_punct(&token, ")")) {
            const long open = matching_open(tokens, index, "(", ")");
            const LexToken *name = at(tokens, open - 1);
            if (open < 0 || name == nullptr || name->kind != LexKind::t_identifier) {
                return {};
            }

            const LexToken *link = at(tokens, open - 2);
            if (is_punct(link, "->") || is_punct(link, "?->")) {
                chain.insert(chain.begin(), ReceiverSegment{ ReceiverSegment::Kind::t_method, name->text, {} });
                index = open - 3;
                continue;
            }

            if (is_punct(link, "::")) {
                long start = 0;
                std::vector<std::string> path = path_ending_at(tokens, open - 3, start);
                if (path.empty()) {
                    return {};
                }
                return finish(ReceiverSegment{ ReceiverSegment::Kind::t_static_call, name->text, std::move(path) });
            }

            return finish(ReceiverSegment{ ReceiverSegment::Kind::t_call, name->text, {} });
        }

        if (is_punct(&token, "]")) {
            const long open = matching_open(tokens, index, "[", "]");
            if (open <= 0) {
                return {};
            }
            chain.insert(chain.begin(), ReceiverSegment{ ReceiverSegment::Kind::t_index, "", {} });
            index = open - 1;
            continue;
        }

        if (token.kind == LexKind::t_identifier) {
            const LexToken *link = at(tokens, index - 1);
            if (is_punct(link, "->") || is_punct(link, "?->")) {
                chain.insert(chain.begin(), ReceiverSegment{ ReceiverSegment::Kind::t_property, token.text, {} });
                index -= 2;
                continue;
            }

            if (is_punct(link, "::")) {
                long start = 0;
                std::vector<std::string> path = path_ending_at(tokens, index - 2, start);
                if (path.empty()) {
                    return {};
                }
                return finish(ReceiverSegment{ ReceiverSegment::Kind::t_static_value, token.text, std::move(path) });
            }

            return {};
        }

        if (token.kind == LexKind::t_variable && token.text.size() > 1) {
            return finish(ReceiverSegment{ ReceiverSegment::Kind::t_variable, token.text, {} });
        }

        return {};
    }

    return {};
}

Compiler::Lsp::CompletionContext Compiler::Lsp::analyze_completion_context(std::string_view text, size_t cursor)
{
    CompletionContext context;
    cursor = std::min(cursor, text.size());

    // the name under the cursor: what is typed of it before, and where it ends after
    size_t start = cursor;
    while (start > 0 && is_ident_char(text[start - 1])) {
        start--;
    }
    if (start > 0 && text[start - 1] == '$') {
        start--;
    }

    size_t finish = cursor;
    while (finish < text.size() && is_ident_char(text[finish])) {
        finish++;
    }

    context.replace_start = start;
    context.replace_end = finish;
    context.prefix = std::string(text.substr(start, cursor - start));

    bool in_code = true;
    const std::vector<LexToken> tokens = lex_echo(text, start, in_code);
    if (!in_code) {
        return context;
    }

    // `3|` and `0x1|` complete to nothing: a number is already finished
    if (!context.prefix.empty() && std::isdigit(static_cast<unsigned char>(context.prefix[0]))) {
        return context;
    }

    const Walk walk = walk_frames(tokens);
    for (const OpenFrame &open : walk.open) {
        context.frames.push_back(open.frame);
    }

    const OpenFrame &top = walk.open.back();
    const LexToken *last = tokens.empty() ? nullptr : &tokens.back();
    const LexToken *before = at(tokens, static_cast<long>(tokens.size()) - 2);
    const bool is_variable = !context.prefix.empty() && context.prefix[0] == '$';

    if (top.use_group && (is_punct(last, "{") || is_punct(last, ","))) {
        context.kind = is_variable ? CompletionKind::t_none : CompletionKind::t_use_path;
        context.path = top.use_path;
        return context;
    }

    if (is_punct(last, "::")) {
        long path_start = 0;
        context.path = path_ending_at(tokens, static_cast<long>(tokens.size()) - 2, path_start);
        if (context.path.empty()) {
            return context;
        }

        const bool is_use = top.statement_begin < tokens.size()
            && is_word(&tokens[top.statement_begin], "use");
        context.kind = is_use ? CompletionKind::t_use_path : CompletionKind::t_static;
        return context;
    }

    if (is_variable) {
        context.kind = CompletionKind::t_variable;
        return context;
    }

    if (is_punct(last, "->") || is_punct(last, "?->")) {
        context.receiver = receiver_chain_of(std::vector<LexToken>(tokens.begin(), tokens.end() - 1));
        context.optional_chain = last->text == "?->";
        context.kind = context.receiver.empty() ? CompletionKind::t_none : CompletionKind::t_member;
        return context;
    }

    if (is_punct(last, ".")) {
        classify_shorthand(context, tokens);
        return context;
    }

    if (is_punct(last, "#[")) {
        context.kind = CompletionKind::t_attribute;
        return context;
    }

    // the far end of a range is a value, but a bare `..` is far more often a range half typed
    if (context.prefix.empty() && (is_punct(last, "..") || is_punct(last, "..="))) {
        return context;
    }

    // `#[if: |` is a condition
    if (is_punct(last, ":") && before != nullptr && before->kind == LexKind::t_identifier
        && is_punct(at(tokens, static_cast<long>(tokens.size()) - 3), "#[")) {
        return context;
    }

    context.kind = CompletionKind::t_identifier;
    context.position = position_of(tokens, walk);
    return context;
}

std::vector<Compiler::Lsp::LexicalVariable> Compiler::Lsp::visible_variables(const CompletionContext &context)
{
    std::vector<LexicalVariable> out;
    std::unordered_set<std::string> seen;

    for (auto frame = context.frames.rbegin(); frame != context.frames.rend(); ++frame) {
        if (frame->kind == FrameKind::t_type) {
            break;
        }

        // within a frame the first mention is the declaration, a later one only assigns
        for (const LexicalVariable &variable : frame->variables) {
            if (seen.insert(variable.name).second) {
                out.push_back(variable);
            }
        }

        if (frame->kind == FrameKind::t_function) {
            break;
        }
    }

    return out;
}

const Compiler::Lsp::LexicalFrame *Compiler::Lsp::this_frame(const CompletionContext &context)
{
    for (size_t i = context.frames.size(); i-- > 1;) {
        const LexicalFrame &frame = context.frames[i];
        if (frame.kind != FrameKind::t_function) {
            continue;
        }

        const LexicalFrame &owner = context.frames[i - 1];
        if (owner.kind == FrameKind::t_type && !frame.is_static) {
            return &owner;
        }

        return nullptr;
    }

    return nullptr;
}

const Compiler::Lsp::LexicalFrame *Compiler::Lsp::enclosing_type_frame(const CompletionContext &context)
{
    for (auto frame = context.frames.rbegin(); frame != context.frames.rend(); ++frame) {
        if (frame->kind == FrameKind::t_type) {
            return &*frame;
        }
    }

    return nullptr;
}

const Compiler::Lsp::LexicalFrame *Compiler::Lsp::enclosing_function_frame(const CompletionContext &context)
{
    for (auto frame = context.frames.rbegin(); frame != context.frames.rend(); ++frame) {
        if (frame->kind == FrameKind::t_function) {
            return &*frame;
        }
    }

    return nullptr;
}
