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

