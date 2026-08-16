#include "Parser/ParserCursor.h"

void Parser::Cursor::skip_until(const std::vector<Token::Type> &types)
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

void Parser::Cursor::skip_until(std::initializer_list<Token::Type> types)
{
    skip_until(std::vector<Token::Type>(types));
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
                // the closing brace of our own scope
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

void Parser::Cursor::try_skip_to_next_statement(std::initializer_list<Token::Type> also_stop_at)
{
    std::vector<Token::Type> stops { Token::Type::t_semicolon, Token::Type::t_close_brace };
    stops.insert(stops.end(), also_stop_at.begin(), also_stop_at.end());

    skip_until(stops);

    // **the terminator is consumed, the brace is not.** a `}` closes a scope this recovery did not
    // open, and the parser that opened it is waiting for exactly that token - so eating it here ends
    // somebody else's block and every statement after it is read one level too deep
    //
    // skipping whatever it landed on takes two recoveries over one failure: a
    // failed call argument recovers past the `;`, the `return` around it then finds no `;` and
    // recovers again, and the second skip takes the function's closing brace. the *body* pass walks
    // a block token by token and is unaffected, so the two passes then disagree about which scope a
    // declaration below is written in - which is two AST nodes for one struct, and
    // AST::FunctionRegistry::claim_declaration_site refusing the second one
    if (is_type(Token::Type::t_semicolon)) {
        skip();
    }
}

void Parser::Cursor::skip_statement()
{
    while (!is_done() && !is_type(Token::Type::t_semicolon)) {
        // a braced group belongs to the statement's *value* - `const H = function() : int32 { ... };`
        // - so the `;` inside it is not the one this statement ends at. skipped as a group through the
        // one owner of "how far does a balanced group extend", which leaves the cursor after its
        // closing brace
        if (is_type(Token::Type::t_open_brace)) {
            skip_balanced_group(Token::Type::t_open_brace, Token::Type::t_close_brace);
            continue;
        }

        skip();
    }

    if (is_type(Token::Type::t_semicolon)) {
        skip();
    }
}
