#include "Parser/TypeParser.h"
#include "AST/ASTValueType.h"


bool Parser::can_parse_type(Parser::Payload &payload)
{
    // a type can be preceded by a const keyword
    size_t offset = 0;
    if (payload.cursor.is_type(Token::Type::t_const)) {
        offset++;
    }

    // a type can be an identifier
    if (payload.cursor.peek_is_type(offset, Token::Type::t_identifier)) {
        return true;
    }

    return false;
}

AST::ValueType get_primitive_type(const std::string &types_string)
{
    if (types_string == "int") {
        return AST::ValueType(AST::ValueTypePrimitive::t_int32);
    } else if (types_string == "int8") {
        return AST::ValueType(AST::ValueTypePrimitive::t_int8);
    } else if (types_string == "int16") {
        return AST::ValueType(AST::ValueTypePrimitive::t_int16);
    } else if (types_string == "int32") {
        return AST::ValueType(AST::ValueTypePrimitive::t_int32);
    } else if (types_string == "int64") {
        return AST::ValueType(AST::ValueTypePrimitive::t_int64);
    } else if (types_string == "uint") {
        return AST::ValueType(AST::ValueTypePrimitive::t_uint32);
    } else if (types_string == "uint8") {
        return AST::ValueType(AST::ValueTypePrimitive::t_uint8);
    } else if (types_string == "uint16") {
        return AST::ValueType(AST::ValueTypePrimitive::t_uint16);
    } else if (types_string == "uint32") {
        return AST::ValueType(AST::ValueTypePrimitive::t_uint32);
    } else if (types_string == "uint64") {
        return AST::ValueType(AST::ValueTypePrimitive::t_uint64);
    } else if (types_string == "float") {
        return AST::ValueType(AST::ValueTypePrimitive::t_float64);
    } else if (types_string == "float32") {
        return AST::ValueType(AST::ValueTypePrimitive::t_float32);
    } else if (types_string == "float64") {
        return AST::ValueType(AST::ValueTypePrimitive::t_float64);
    } else if (types_string == "bool") {
        return AST::ValueType(AST::ValueTypePrimitive::t_bool);
    } else if (types_string == "void") {
        return AST::ValueType(AST::ValueTypePrimitive::t_void);
    }

    return AST::ValueType::make_unknown();
}

AST::TypeNode &Parser::parse_type(Parser::Payload &payload)
{
    bool is_const = false;

    if (payload.cursor.is_type(Token::Type::t_const)) {
        is_const = true;
        payload.cursor.skip();
    }

    auto token = payload.cursor.current();
    auto primitive_type = get_primitive_type(token.value());

    payload.cursor.skip();

    auto &node = payload.context.emplace_node<AST::TypeNode>(primitive_type, token);
    node.is_const = is_const;

    return node;
}