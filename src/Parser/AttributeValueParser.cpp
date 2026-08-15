#include "Parser/AttributeValueParser.h"

#include "AST/ASTStringLiteral.h"

#include <fmt/core.h>

namespace
{
    // everything the value's own walk consumed. TokenSpan::upto owns the "a cursor sits one past the
    // last token" conversion, so no site here does it by hand
    TokenSpan span_between(const Parser::Cursor &cursor, const Parser::Cursor::Snapshot &start)
    {
        return TokenSpan::upto(cursor.tokens, start.index, cursor.snapshot().index);
    }

    void refuse(Parser::Payload &payload, const TokenReference &at, const std::string &message)
    {
        payload.collector.collect_issue<AST::Issue::GenericError>(payload.context.code_ref(at), message);
    }

    TokenReference here(const Parser::Cursor &cursor)
    {
        return cursor.here();
    }

    // is the token `offset` ahead a bare word - an identifier, or a keyword spelled like one.
    //
    // **the three places this grammar reads a name all go through it**: whether a value starts here, whether
    // a name is a *tag* on the value behind it, and the bare-name atom itself. They have to agree, or
    // `#[target: test { ... }]` reads its tag as an atom and then meets a '{' it has no arm for - which is
    // exactly what a keyword being added to the lexer did to it
    bool at_a_word(const Parser::Cursor &cursor, size_t offset = 0)
    {
        if (!cursor.is_valid_offset(offset)) {
            return false;
        }

        return token_spells_a_word(cursor.peek(offset).value());
    }

    bool parse_atom(Parser::Payload &payload, AST::AttributeValue &out);

    // the three sites that accept a number have to agree, or `#[foo: 0b1]` is a token the
    // grammar does not know - the same class of miss hex used to be
    bool is_number_token(Token::Type type)
    {
        return type == Token::Type::t_integer_literal
            || type == Token::Type::t_hex_literal
            || type == Token::Type::t_binary_literal
            || type == Token::Type::t_floating_literal;
    }

    bool parse_list(Parser::Payload &payload, AST::AttributeValue &out)
    {
        Parser::Cursor &cursor = payload.cursor;
        const TokenReference opener = cursor.current();

        cursor.skip(); // `[`

        while (!cursor.is_type(Token::Type::t_close_bracket)) {
            AST::AttributeValue item;

            if (!Parser::parse_attribute_value(payload, item)) {
                return false;
            }

            out.items.push_back(std::move(item));

            if (cursor.is_type(Token::Type::t_comma)) {
                cursor.skip();
                continue;
            }

            break;
        }

        if (!cursor.is_type(Token::Type::t_close_bracket)) {
            refuse(payload, here(cursor), fmt::format(
                "this list is missing its ']' - the '[' is on line {}.", opener.line()));
            return false;
        }

        cursor.skip(); // `]`
        return true;
    }

    bool parse_record(Parser::Payload &payload, AST::AttributeValue &out)
    {
        Parser::Cursor &cursor = payload.cursor;
        const TokenReference opener = cursor.current();

        cursor.skip(); // `{`

        while (!cursor.is_type(Token::Type::t_close_brace)) {
            if (!cursor.is_type(Token::Type::t_identifier)) {
                refuse(payload, here(cursor), "a record key has to be a name, written 'key: value'.");
                return false;
            }

            const TokenReference key = cursor.current();

            // **the second one, not the first.** a duplicate is a decision the author made twice and
            // pointing at the later spelling is what tells them which of the two they can delete
            if (out.find_field(key.value()) != nullptr) {
                refuse(payload, key, fmt::format("'{}' is written twice in this record.", key.value()));
                return false;
            }

            cursor.skip(); // the key

            if (!Parser::expect_attribute_colon(payload)) {
                return false;
            }

            AST::AttributeField field;
            field.key_span = TokenSpan::of(key);
            field.key = key.value();

            if (!Parser::parse_attribute_value(payload, field.value)) {
                return false;
            }

            out.fields.push_back(std::move(field));

            if (cursor.is_type(Token::Type::t_comma)) {
                cursor.skip();
                continue;
            }

            break;
        }

        if (!cursor.is_type(Token::Type::t_close_brace)) {
            refuse(payload, here(cursor), fmt::format(
                "this record is missing its '}}' - the '{{' is on line {}.", opener.line()));
            return false;
        }

        cursor.skip(); // `}`
        return true;
    }

    bool parse_number(Parser::Payload &payload, AST::AttributeValue &out, bool negated)
    {
        Parser::Cursor &cursor = payload.cursor;
        const TokenReference token = cursor.current();
        const std::string &written = token.value();

        if (token.type() == Token::Type::t_floating_literal) {
            // a trailing `f` is the language's float32 marker; an attribute has one number kind and no
            // types to choose between, so it is read off and forgotten
            const std::string digits = written.back() == 'f' ? written.substr(0, written.size() - 1) : written;

            out.kind = AST::AttributeValueKind::t_float;
            out.number = std::stod(digits) * (negated ? -1.0 : 1.0);
            cursor.skip();
            return true;
        }

        const int base = token.type() == Token::Type::t_hex_literal ? 16
            : token.type() == Token::Type::t_binary_literal ? 2
            : 10;

        // `0x` is accepted by stoll at base 16; `0b` is not accepted at base 2
        const std::string digits = token.type() == Token::Type::t_binary_literal
            ? written.substr(2)
            : written;

        try {
            out.kind = AST::AttributeValueKind::t_int;
            out.integer = static_cast<int64_t>(std::stoll(digits, nullptr, base)) * (negated ? -1 : 1);
        } catch (const std::exception &) {
            refuse(payload, token, fmt::format("'{}' does not fit in a number.", written));
            return false;
        }

        cursor.skip();
        return true;
    }

    bool parse_atom(Parser::Payload &payload, AST::AttributeValue &out)
    {
        Parser::Cursor &cursor = payload.cursor;

        if (cursor.is_type(Token::Type::t_string_literal)) {
            const TokenReference token = cursor.current();

            out.kind = AST::AttributeValueKind::t_string;

            // AST::decode_string_literal, not a substr of the token's interior: it is the one answer to
            // what bytes a literal denotes, and it is the only thing that reports a bad escape rather
            // than carrying it through into a path or a linker word
            if (std::optional<AST::StringLiteralError> error = AST::decode_string_literal(token.value(), out.text)) {
                refuse(payload, token, error->message);
                return false;
            }

            cursor.skip();
            return true;
        }

        if (cursor.is_type(Token::Type::t_bool_literal)) {
            out.kind = AST::AttributeValueKind::t_bool;
            out.boolean = cursor.current().value() == "true";
            cursor.skip();
            return true;
        }

        if (!cursor.is_done() && is_number_token(cursor.current().type())) {
            return parse_number(payload, out, false);
        }

        if (cursor.is_type(Token::Type::t_op_sub)) {
            cursor.skip(); // `-`

            if (cursor.is_done() || !is_number_token(cursor.current().type())) {
                refuse(payload, here(cursor), "a '-' in an attribute has to be followed by a number.");
                return false;
            }

            return parse_number(payload, out, true);
        }

        // **a bare name, and a keyword spelled like a word is one.** `#[target: test]` names a target kind
        // with a word Echo also keeps as a keyword, and inside `#[ ]` there is nothing for that keyword's
        // meaning to collide with: the region resolves to no declaration. token_spells_a_word is the one
        // answer to that, shared with the attribute *name* the conditional filter reads
        if (at_a_word(cursor)) {
            out.kind = AST::AttributeValueKind::t_name;
            out.text = cursor.current().value();
            cursor.skip();
            return true;
        }

        if (cursor.is_type(Token::Type::t_open_bracket)) {
            out.kind = AST::AttributeValueKind::t_list;
            return parse_list(payload, out);
        }

        if (cursor.is_type(Token::Type::t_open_brace)) {
            out.kind = AST::AttributeValueKind::t_record;
            return parse_record(payload, out);
        }

        refuse(payload, here(cursor), fmt::format(
            "'{}' is not something an attribute can say - a value is a name, a string, a number, "
            "true or false, a '[' list or a '{{' record.",
            cursor.is_done() ? std::string("end of file") : cursor.current().value()));
        return false;
    }
};

bool Parser::starts_attribute_value(const Parser::Cursor &cursor, size_t offset)
{
    return cursor.peek_is_type(offset, Token::Type::t_string_literal)
        || cursor.peek_is_type(offset, Token::Type::t_integer_literal)
        || cursor.peek_is_type(offset, Token::Type::t_hex_literal)
        || cursor.peek_is_type(offset, Token::Type::t_binary_literal)
        || cursor.peek_is_type(offset, Token::Type::t_floating_literal)
        || cursor.peek_is_type(offset, Token::Type::t_bool_literal)
        || at_a_word(cursor, offset)
        || cursor.peek_is_type(offset, Token::Type::t_op_sub)
        || cursor.peek_is_type(offset, Token::Type::t_open_bracket)
        || cursor.peek_is_type(offset, Token::Type::t_open_brace);
}

bool Parser::expect_attribute_colon(Parser::Payload &payload)
{
    // `#[name:$value]` lexes its `:$` as the pointer-of operator - the one place in the grammar where a
    // colon immediately followed by a `$` means something else. a space disambiguates, and saying so
    // beats letting the attribute fail as a mysteriously missing colon
    if (payload.cursor.is_type(Token::Type::t_ptr_of)) {
        refuse(payload, payload.cursor.current(),
            "Write a space after ':' in an attribute value - ':$' is the pointer-of operator");
        return false;
    }

    if (!payload.cursor.is_type(Token::Type::t_colon)) {
        payload.collect_unexpected_token(Token::Type::t_colon);
        return false;
    }

    payload.cursor.skip(); // `:`
    return true;
}

bool Parser::parse_attribute_value(Parser::Payload &payload, AST::AttributeValue &out)
{
    Parser::Cursor &cursor = payload.cursor;
    const Parser::Cursor::Snapshot start = cursor.snapshot();

    // `name atom` is a tag applied to a payload; a name with nothing that starts a value after it is
    // just a name. that is the whole of the lookahead, and the reason `#[core: array]` reads as one
    const bool is_tagged = at_a_word(cursor)
        && Parser::starts_attribute_value(cursor, 1);

    TokenSpan tag_span;

    if (is_tagged) {
        tag_span = TokenSpan::of(cursor.current());
        cursor.skip(); // the tag
    }

    if (!parse_atom(payload, out)) {
        return false;
    }

    out.tag_span = tag_span;
    out.span = span_between(cursor, start);

    return true;
}
