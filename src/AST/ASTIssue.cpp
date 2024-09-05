#include "AST/ASTIssue.h"
#include "AST/VarDeclNode.h"

#include <fmt/core.h>

#define ISSUE_MESSAGE_FNC(className) \
std::string AST::Issue::className::message() const

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
    return fmt::format(
        "The variable '{}' is already declared on line {} column {} and cannot be redeclared with a different type",
        previous_declaration->name(),
        previous_declaration->token_varname.line(),
        previous_declaration->token_varname.column());
}

ISSUE_MESSAGE_FNC(UnknownVariable)
{
    return fmt::format("The variable '{}' is not declared in the current scope", variable_name);
}

ISSUE_MESSAGE_FNC(UnknownFunction)
{
    return fmt::format("The function '{}' could not be found", function_name);
}

ISSUE_MESSAGE_FNC(LossOfPrecision)
{
    return fmt::format("This operation results in a loss of precision: {}", _message);
}

ISSUE_MESSAGE_FNC(InvalidTypeConversion)
{
    return fmt::format("Invalid type conversion: {}", _message);
}

ISSUE_MESSAGE_FNC(ConstViolation)
{
    return fmt::format("Const violation: {}", _message);
}

ISSUE_MESSAGE_FNC(IntegerOverflow)
{
    return fmt::format("Integer overflow: {}", _message);
}

ISSUE_MESSAGE_FNC(IntegerUnderflow)
{
    return fmt::format("Integer underflow: {}", _message);
}

ISSUE_MESSAGE_FNC(UnknownMember)
{
    return fmt::format("The struct '{}' has no member named '{}'", struct_name, member_name);
}

ISSUE_MESSAGE_FNC(ArgumentTypeMismatch)
{
    return _message;
}

ISSUE_MESSAGE_FNC(UnresolvedTypeParameter)
{
    return _message;
}

ISSUE_MESSAGE_FNC(UnsatisfiedTypeConstraint)
{
    return _message;
}