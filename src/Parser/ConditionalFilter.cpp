#include "Parser/ConditionalFilter.h"

#include "Parser/ParserCursor.h"

#include <fmt/core.h>

#include <string>
#include <vector>

namespace
{
    using Compiler::TargetFacts;

    std::string locate(uint32_t line, const std::string &message)
    {
        return fmt::format("line {}: {}", line, message);
    }

    // one directive, located: which of the four it is, where its condition sits, and where the whole thing
    // ends. `condition_end` is exclusive and equals `condition_start` when there is no condition
    struct Directive
    {
        std::string name;
        size_t condition_start = 0;
        size_t condition_end = 0;
        size_t after = 0;          // the first index past the closing `]`
        uint32_t line = 0;
    };

    // the *name* of a `#[...]` at `index`, or "" when there is no attribute there at all.
    //
    // two token types, because `if` and `else` are keywords - the reason this reads a value rather than
    // matching a type
    std::string attribute_name_at(const TokenCollection &tokens, size_t index)
    {
        if (index + 2 >= tokens.size()) {
            return "";
        }

        if (!tokens.tokens[index].is_a(Token::Type::t_hash)
            || !tokens.tokens[index + 1].is_a(Token::Type::t_open_bracket)) {
            return "";
        }

        const Token &name = tokens.tokens[index + 2];

        if (!name.is_one_of({ Token::Type::t_identifier, Token::Type::t_if, Token::Type::t_else })) {
            return "";
        }

        return tokens.token_values[index + 2];
    }

    // the index one past the token closing the balanced `open ... close` group that starts at `index`, or
    // npos when it is never closed.
    //
    // **Parser::Cursor::skip_balanced_group does the counting**, which is the one place in this compiler
    // that knows how - a Cursor is constructible straight over a TokenCollection, so it is reachable from
    // here with no parse payload to build. It lands *past* the closing token or at the end of the
    // collection, and only the first of those is a group, which is what the token check below asks
    size_t balanced_group_end(
        const TokenCollection &tokens, size_t index, Token::Type open, Token::Type close)
    {
        Parser::Cursor cursor(tokens, index);
        cursor.skip_balanced_group(open, close);

        const size_t after = cursor.snapshot().index;

        if (after <= index || !tokens.tokens[after - 1].is_a(close)) {
            return std::string::npos;
        }

        return after;
    }

    // the index one past the `]` closing the `#[...]` at `index`, or npos when it is never closed.
    //
    // brackets are **counted**, where read_directive below scans to the first `]`. Both are right: a
    // condition's grammar has no bracket in it, and a general attribute's value may be a list, so
    // `#[sources: ["a", "b"]]` closes three of them
    size_t attribute_end(const TokenCollection &tokens, size_t index)
    {
        return balanced_group_end(
            tokens, index + 1, Token::Type::t_open_bracket, Token::Type::t_close_bracket);
    }

    // could a `#[...]* test <name> { ... }` begin at this token at all - the two token types locate_test_block
    // below can answer anything but `t_absent` for. Its own predicate so the walk asks the cheap question
    // before the scan, and so the two cannot disagree about which they are
    bool starts_a_test_block(const Token &token)
    {
        return token.is_one_of({ Token::Type::t_hash, Token::Type::t_test });
    }

    // three answers, so "there is no test here" and "there is one and it is malformed" are not one value
    enum class TestBlockScan
    {
        t_absent,
        t_located,
        t_malformed
    };

    // is `#[...]* test <name> { ... }` what starts at `index`, and where does it end?
    //
    // **the attributes are found by scanning forward from the `#`**, never by rewinding the write cursor
    // back over ones already kept. That is not only cheaper - a `#[group: "..."]` left behind by a dropped
    // test does not go missing, it attaches to whatever declaration follows and does so in silence, which
    // is the failure this lookahead exists to prevent.
    //
    // a malformed *header* is a located error even in a build that drops the block, because the header is
    // the only thing that says where the block ends. A malformed *body* is invisible, which is the same
    // price every excluded region pays
    TestBlockScan locate_test_block(
        const TokenCollection &tokens, size_t index, size_t &out_after, std::string &out_error)
    {
        size_t cursor = index;

        // the attribute run. a directive stops it - `#[if: ...]` belongs to the frame stack and is the
        // outer loop's business, not this block's
        while (true) {
            const std::string name = attribute_name_at(tokens, cursor);

            if (name.empty() || Parser::ConditionalDirective::is_directive(name)) {
                break;
            }

            const size_t after = attribute_end(tokens, cursor);

            if (after == std::string::npos) {
                return TestBlockScan::t_absent;
            }

            cursor = after;
        }

        if (cursor >= tokens.size() || !tokens.tokens[cursor].is_a(Token::Type::t_test)) {
            return TestBlockScan::t_absent;
        }

        const uint32_t line = tokens.tokens[cursor].line;
        cursor++;

        if (cursor >= tokens.size() || !tokens.tokens[cursor].is_a(Token::Type::t_identifier)) {
            out_error = locate(line, "'test' must be followed by the test's name");
            return TestBlockScan::t_malformed;
        }

        cursor++;

        if (cursor >= tokens.size() || !tokens.tokens[cursor].is_a(Token::Type::t_open_brace)) {
            out_error = locate(line, "a test's name must be followed by its body, '{'");
            return TestBlockScan::t_malformed;
        }

        // counting braces is sound at token level: an interpolated string's holes never produce one, the
        // lexer having consumed them itself while lexing the literal
        const size_t after = balanced_group_end(
            tokens, cursor, Token::Type::t_open_brace, Token::Type::t_close_brace);

        if (after == std::string::npos) {
            out_error = locate(line, "this test's body is never closed - add '}'");
            return TestBlockScan::t_malformed;
        }

        out_after = after;
        return TestBlockScan::t_located;
    }

    // reads the directive at `index`, which the caller has already established is one.
    //
    // the condition runs to the first `]`, which is safe because a condition cannot contain one: the
    // grammar is identifiers, comparisons, `&&`, `||`, `!` and parentheses
    bool read_directive(
        const TokenCollection &tokens, size_t index, Directive &out, std::string &out_error)
    {
        out.name = attribute_name_at(tokens, index);
        out.line = tokens.tokens[index].line;

        size_t cursor = index + 3;

        // the `:` when there is one - `#[else]` and `#[end]` carry no condition, and then the condition is
        // the empty range starting here
        if (cursor < tokens.size() && tokens.tokens[cursor].is_a(Token::Type::t_colon)) {
            cursor++;
        }

        out.condition_start = cursor;

        while (cursor < tokens.size() && !tokens.tokens[cursor].is_a(Token::Type::t_close_bracket)) {
            cursor++;
        }

        if (cursor >= tokens.size()) {
            out_error = locate(out.line,
                fmt::format("'#[{}' is missing its closing ']'", out.name));
            return false;
        }

        out.condition_end = cursor;
        out.after = cursor + 1;

        return true;
    }

    // the condition evaluator: recursive descent over [cursor, end), standard precedence.
    //
    //   or    := and ( '||' and )*
    //   and   := unary ( '&&' unary )*
    //   unary := '!' unary | '(' or ')' | axis '=='|'!=' identifier | identifier
    //
    // its own parser rather than the expression parser's, and it has to be: this runs before pass 1, and
    // `darwin` is not an Echo expression - there is nothing to resolve it against and never will be
    struct ConditionEvaluator
    {
        const TokenCollection &tokens;
        const TargetFacts &facts;
        size_t cursor;
        size_t end;
        uint32_t line;
        std::string error;

        bool at_end() const {
            return cursor >= end;
        }

        bool is(Token::Type type) const {
            return !at_end() && tokens.tokens[cursor].is_a(type);
        }

        bool fail(const std::string &message) {
            if (error.empty()) {
                error = locate(line, message);
            }
            return false;
        }

        bool parse_or(bool &out)
        {
            if (!parse_and(out)) {
                return false;
            }

            while (is(Token::Type::t_logical_or)) {
                cursor++;

                bool right = false;
                if (!parse_and(right)) {
                    return false;
                }

                // no short circuit, deliberately: both sides are pure lookups over TargetFacts, and
                // evaluating one of them can never be wrong or expensive. Skipping it would only mean an
                // unknown value on the right went unreported
                out = out || right;
            }

            return true;
        }

        bool parse_and(bool &out)
        {
            if (!parse_unary(out)) {
                return false;
            }

            while (is(Token::Type::t_logical_and)) {
                cursor++;

                bool right = false;
                if (!parse_unary(right)) {
                    return false;
                }

                out = out && right;
            }

            return true;
        }

        bool parse_unary(bool &out)
        {
            if (is(Token::Type::t_exclamation)) {
                cursor++;

                bool inner = false;
                if (!parse_unary(inner)) {
                    return false;
                }

                out = !inner;
                return true;
            }

            if (is(Token::Type::t_open_paren)) {
                cursor++;

                if (!parse_or(out)) {
                    return false;
                }

                if (!is(Token::Type::t_close_paren)) {
                    return fail("a '(' in a condition is missing its ')'");
                }

                cursor++;
                return true;
            }

            return parse_term(out);
        }

        // an axis test or a flag. which it is, is decided by the *name*: the two axes are reserved, so a
        // bare `os` is a mistake rather than a flag lookup
        bool parse_term(bool &out)
        {
            if (!is(Token::Type::t_identifier)) {
                if (at_end()) {
                    return fail("a condition ended where a name was expected");
                }

                return fail(fmt::format(
                    "'{}' is not a name a condition can test", tokens.token_values[cursor]));
            }

            const std::string name = tokens.token_values[cursor];
            cursor++;

            if (!TargetFacts::is_axis(name)) {
                // **a name followed by a comparison was meant as an axis**, so it is reported as an
                // unknown one rather than read as a flag that happens to be false. Without this the
                // grammar accepts `cpu` as a define, finds a stray `==`, and blames the operator - which
                // tells the author nothing about the mistake they actually made
                if (is(Token::Type::t_logical_eq) || is(Token::Type::t_logical_neq)) {
                    return fail(fmt::format(
                        "unknown condition axis '{}', expected one of: {}",
                        name, TargetFacts::axis_names()));
                }

                // **absent is false, not an error.** A condition has to be able to ask about a flag
                // nobody set, or `#[if: TRACE]` could only be written by someone who had already passed
                // `--define TRACE` - and then it could never take its else arm
                out = facts.has_define(name);
                return true;
            }

            const bool negated = is(Token::Type::t_logical_neq);

            if (!negated && !is(Token::Type::t_logical_eq)) {
                return fail(fmt::format(
                    "'{}' is a condition axis and needs a comparison - write `{} == <value>`",
                    name, name));
            }

            cursor++;

            if (!is(Token::Type::t_identifier)) {
                return fail(fmt::format("'{}' is compared against a bare name, not a literal", name));
            }

            const std::string value = tokens.token_values[cursor];
            cursor++;

            // **both halves of an axis test are checked**, by the type that owns the vocabulary:
            // `#[if: os == darwn]` is a typo that would otherwise be a silently vanishing block, and there
            // is nothing else in the compiler that would ever notice
            bool matched = false;
            std::string reason;

            if (!facts.axis_equals(name, value, matched, reason)) {
                return fail(reason);
            }

            out = negated ? !matched : matched;
            return true;
        }
    };

    bool evaluate_condition(
        const TokenCollection &tokens,
        const Directive &directive,
        const TargetFacts &facts,
        bool &out_value,
        std::string &out_error)
    {
        if (directive.condition_start >= directive.condition_end) {
            out_error = locate(directive.line,
                fmt::format("'#[{}: ...]' needs a condition", directive.name));
            return false;
        }

        ConditionEvaluator evaluator{
            tokens, facts, directive.condition_start, directive.condition_end, directive.line, "" };

        if (!evaluator.parse_or(out_value)) {
            out_error = evaluator.error;
            return false;
        }

        // a trailing token means the grammar stopped early - `os == darwin linux` rather than
        // `os == darwin || os == linux`. reported, because a condition read halfway is a condition whose
        // answer is not the one written
        if (!evaluator.at_end()) {
            out_error = locate(directive.line, fmt::format(
                "unexpected '{}' after the condition", tokens.token_values[evaluator.cursor]));
            return false;
        }

        return true;
    }

    // one open `#[if]`. `taken` is what makes an `#[elif]` chain exclusive: once an arm has been kept,
    // every later arm is skipped whatever its condition says
    struct Frame
    {
        bool taken = false;
        bool emitting = false;
        bool seen_else = false;
        uint32_t line = 0;
    };
};

bool Parser::ConditionalDirective::is_directive(const std::string &name)
{
    return name == k_if || name == k_elif || name == k_else || name == k_end;
}

bool Parser::filter_conditional_tokens(
    TokenCollection &tokens,
    size_t from,
    const Compiler::TargetFacts &facts,
    std::string &out_error
)
{
    std::vector<Frame> frames;

    // **compacted in place, behind the read cursor.** an erase per dropped token is quadratic over a file,
    // and a rebuild into side vectors copies every token of every file - including the overwhelming
    // majority that carry no directive at all and come out byte for byte identical. a write cursor is
    // neither: it is one self-assignment per kept token until something is dropped, and the two parallel
    // vectors still advance at one site, which is what kept a rebuild honest
    size_t write = from;

    // emitting when every open frame is. one predicate rather than a running flag, because an `#[elif]`
    // deep in a nest changes only its own frame and the answer has to be recomputed from all of them
    const auto emitting = [&frames]() {
        for (const Frame &frame : frames) {
            if (!frame.emitting) {
                return false;
            }
        }
        return true;
    };

    size_t index = from;

    while (index < tokens.size()) {
        const std::string name = attribute_name_at(tokens, index);

        if (!ConditionalDirective::is_directive(name)) {
            // **a test block this build does not compile never reaches pass 1.** Asked only while emitting,
            // so a test inside a region another platform owns is dropped by that region and the conditions
            // inside it are still evaluated - which is what keeps a typo in one reportable.
            //
            // the two cheap questions first, and the token one is why: this branch is every token of every
            // file, and a test block starts with one of exactly two token types. `emitting()` walks the
            // frame stack, so asking it per token for a scan that could not have matched is the one cost
            // this loop cannot afford
            if (!facts.tests && starts_a_test_block(tokens.tokens[index]) && emitting()) {
                size_t after = 0;

                switch (locate_test_block(tokens, index, after, out_error)) {
                case TestBlockScan::t_malformed:
                    return false;

                case TestBlockScan::t_located:
                    index = after;
                    continue;

                case TestBlockScan::t_absent:
                    break;
                }
            }

            // **an attribute is one step, interior included.** Its tokens are compile-time data rather than
            // statements, so nothing in this loop may read one as a keyword: `#[target: test]` in a manifest
            // holds a `t_test` that is a word naming a target kind, and examining it token by token had this
            // filter refuse every manifest declaring one.
            //
            // an attribute whose `]` is missing falls back to a single token, so a malformed one is still
            // reported by the parser rather than swallowing the rest of the file here
            size_t next = index + 1;

            if (!name.empty()) {
                const size_t after = attribute_end(tokens, index);

                if (after != std::string::npos) {
                    next = after;
                }
            }

            if (emitting()) {
                // `write <= index` always, so this only ever reads tokens the walk has passed
                for (size_t at = index; at < next; at++) {
                    if (write != at) {
                        tokens.tokens[write] = tokens.tokens[at];
                        tokens.token_values[write] = std::move(tokens.token_values[at]);
                    }

                    write++;
                }
            }

            index = next;
            continue;
        }

        Directive directive;
        if (!read_directive(tokens, index, directive, out_error)) {
            return false;
        }

        if (name == ConditionalDirective::k_if) {
            Frame frame;
            frame.line = directive.line;

            // **the condition is evaluated even inside a region being skipped.** That is what makes a
            // typo in a nested condition reportable on a platform whose outer arm was not taken - and it
            // is safe, because evaluating one is a lookup over TargetFacts with no side effects
            bool value = false;
            if (!evaluate_condition(tokens, directive, facts, value, out_error)) {
                return false;
            }

            frame.taken = value;
            frame.emitting = value;
            frames.push_back(frame);

            index = directive.after;
            continue;
        }

        if (frames.empty()) {
            out_error = locate(directive.line,
                fmt::format("'#[{}]' has no open '#[if: ...]'", name));
            return false;
        }

        Frame &frame = frames.back();

        if (name == ConditionalDirective::k_end) {
            frames.pop_back();
            index = directive.after;
            continue;
        }

        if (frame.seen_else) {
            out_error = locate(directive.line, fmt::format(
                "'#[{}]' comes after the '#[else]' of the '#[if: ...]' on line {}",
                name, frame.line));
            return false;
        }

        if (name == ConditionalDirective::k_else) {
            frame.seen_else = true;
            frame.emitting = !frame.taken;
            frame.taken = true;

            index = directive.after;
            continue;
        }

        // `#[elif: ...]` - evaluated whether or not an earlier arm was taken, so that an unknown value in
        // it is still caught, and then kept only if none was
        bool value = false;
        if (!evaluate_condition(tokens, directive, facts, value, out_error)) {
            return false;
        }

        frame.emitting = !frame.taken && value;
        frame.taken = frame.taken || value;

        index = directive.after;
    }

    if (!frames.empty()) {
        out_error = locate(frames.back().line, "'#[if: ...]' is never closed - add '#[end]'");
        return false;
    }

    // `erase` rather than `resize`: Token has no default constructor, so a resize that could grow does not
    // compile - and this only ever shrinks, which is what makes erase-to-the-end the honest spelling
    tokens.tokens.erase(tokens.tokens.begin() + static_cast<std::ptrdiff_t>(write), tokens.tokens.end());
    tokens.token_values.erase(
        tokens.token_values.begin() + static_cast<std::ptrdiff_t>(write), tokens.token_values.end());

    return true;
}
