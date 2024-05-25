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

void Parser::Cursor::try_skip_to_next_statement()
{
    skip_until({ Token::Type::t_semicolon, Token::Type::t_close_brace });
    skip();
}