#include "AST/ASTIssue.h"
#include "AST/VarDeclNode.h"

#include <format>

#define ISSUE_MESSAGE_FNC(className) \
const std::string AST::Issue::className::message() const

ISSUE_MESSAGE_FNC(GenericError) {
    return _message;
}

ISSUE_MESSAGE_FNC(GenericWarning) {
    return _message;
}

ISSUE_MESSAGE_FNC(GenericInfo) {
    return _message;
}

ISSUE_MESSAGE_FNC(UnexpectedToken)
{
    if (expected == Token::Type::t_unknown) {
        return "Unexpected token '" + token_type_string(actual) + "' found";
    }

    return "Unexpected token '" + token_type_string(actual) + "' found. Expected '" + token_type_string(expected) + "'";
}

ISSUE_MESSAGE_FNC(VariableRedeclaration)
{
    return std::format("The const variable '{}' is already declared on line {} column {} and cannot be modified", 
        previous_declaration->name(), 
        previous_declaration->token_varname.line(), 
        previous_declaration->token_varname.column());
}

ISSUE_MESSAGE_FNC(UnknownVariable)
{
    return std::format("The variable '{}' is not declared in the current scope", variable_name);
}

ISSUE_MESSAGE_FNC(LossOfPrecision)
{
    return std::format("This operation results in a loss of precision: {}", _message);
}

ISSUE_MESSAGE_FNC(InvalidTypeConversion)
{
    return std::format("Invalid type conversion: {}", _message);
}

ISSUE_MESSAGE_FNC(IntegerOverflow)
{
    return std::format("Integer overflow: {}", _message);
}

ISSUE_MESSAGE_FNC(IntegerUnderflow)
{
    return std::format("Integer underflow: {}", _message);
}