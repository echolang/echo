#include "AST/LiteralValueNode.h"

const std::string AST::LiteralValueNode::literal_type_description()
{
    switch (literal_token.type())
    {
        case Token::Type::t_integer_literal:
            return "int";
        case Token::Type::t_floating_literal:
            return "float";
        case Token::Type::t_string_literal:
            return "string";
        case Token::Type::t_binary_literal:
            return "binary";
        case Token::Type::t_hex_literal:
            return "hex";
        case Token::Type::t_bool_literal:
            return "bool";
        default:
            break;
    }
    
    return "unknown";
}