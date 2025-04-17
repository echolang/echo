#include "Parser/ParserCursor.h"

void Parser::Cursor::skip_until(std::initializer_list<Token::Type> types)
{
    while (!is_done()) {
        for (auto type : types) {
            if (current().type() == type) {
                return;
            }
        }
        skip();
    }
}

TokenSlice Parser::Cursor::slice(const Snapshot &start, const Snapshot &end) const
{
    return tokens.slice(start.index, end.index);
}

void Parser::Cursor::skip_till_end_of_scope()
{
    int depth = 0; // count nested braces inside our scope
    while (!is_done()) {
        if (current().type() == Token::Type::t_open_brace) {
            depth++;
        }
        else if (current().type() == Token::Type::t_close_brace) {
            if (depth == 0) {
                // This is the closing brace of our scope
                skip(); // skip past the closing brace
                return;
            } else {
                depth--;
            }
        }
        skip();
    }
}

void Parser::Cursor::skip_balanced_group(Token::Type open, Token::Type close)
{
    if (!is_type(open)) {
        return;
    }

    int depth = 0;

    while (!is_done()) {
        if (is_type(open)) {
            depth++;
        } else if (is_type(close)) {
            depth--;

            if (depth == 0) {
                skip(); // past the closing token
                return;
            }
        }

        skip();
    }
}

void Parser::Cursor::try_skip_to_next_statement()
{
    skip_until({ Token::Type::t_semicolon, Token::Type::t_close_brace });
    skip();
}