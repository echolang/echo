#include "Parser/TypeParser.h"


bool Parser::can_parse_type(Parser::Payload &payload)
{
    if (payload.cursor.is_type(Token::Type::t_identifier)) {
        return true;
    }

    return false;
}

AST::TypeNode &Parser::parse_type(Parser::Payload &payload)
{
    // 
    // TODO: insert return statement here
}