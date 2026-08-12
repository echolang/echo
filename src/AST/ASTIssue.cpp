#include "AST/ASTIssue.h"
#include "AST/VarDeclNode.h"

#include <fmt/core.h>

#define ISSUE_MESSAGE_FNC(className) \
std::string AST::Issue::className::message() const

std::vector<AST::IssueLabel> AST::label_outside_primary(
    const AST::CodeRef &code_ref,
    const TokenReference &at,
    std::string message
)
{
    const auto [primary_line, primary_end] = code_ref.line_range();
    const uint32_t label_line = at.line();

    if (label_line >= primary_line && label_line <= primary_end) {
        return {};
    }

    return { AST::IssueLabel { at.make_slice(), std::move(message) } };
}

ISSUE_MESSAGE_FNC(GenericError)
{
    return _message;
}

ISSUE_MESSAGE_FNC(GenericWarning)
{
    return _message;
}

ISSUE_MESSAGE_FNC(GenericInfo)
{
    return _message;
}

ISSUE_MESSAGE_FNC(UnexpectedToken)
{
    if (expected == Token::Type::t_unknown) {
        return "Unexpected token '" + token_type_string(actual) + "' found";
    }

    return "Unexpected token '" + token_type_string(actual) + "' found. Expected '" + token_type_string(expected) + "'";
}

std::string AST::Issue::UnexpectedToken::primary_label() const
{
    if (expected == Token::Type::t_unknown) {
        return "unexpected here";
    }

    return "expected " + token_type_string(expected);
}

// the line and column the message used to carry are a *label* now - the renderer shows the previous
// declaration in its own frame, quoting the line. Same fact, and the reader no longer has to go and find
// it. See AST::to_diagnostic for how the token is resolved back to a file
ISSUE_MESSAGE_FNC(VariableRedeclaration)
{
    return fmt::format(
        "The variable '{}' is already declared and cannot be redeclared with a different type",
        previous_declaration->name());
}

std::vector<AST::IssueLabel> AST::Issue::VariableRedeclaration::labels() const
{
    return { IssueLabel { previous_declaration->token_varname.make_slice(), "first declared here" } };
}

ISSUE_MESSAGE_FNC(TypeRedeclaration)
{
    return fmt::format("The type '{}' is already declared", type_name);
}

std::vector<AST::IssueLabel> AST::Issue::TypeRedeclaration::labels() const
{
    // naming the surviving declaration matters: every follow-on error the user sees comes from the
    // first declaration's layout, not from the one they are looking at
    return {
        IssueLabel {
            previous_declaration_token.make_slice(),
            "first declared here, and this is the one that is used" } };
}

ISSUE_MESSAGE_FNC(UnknownVariable)
{
    return fmt::format("The variable '{}' is not declared in the current scope", variable_name);
}

ISSUE_MESSAGE_FNC(UnknownFunction)
{
    return fmt::format("The function '{}' could not be found", function_name);
}

ISSUE_MESSAGE_FNC(UnknownConstant)
{
    return fmt::format(
        "Unknown constant '{}'. A variable carries a '$' - did you mean '${}'? A constant is declared at "
        "file, namespace or struct scope, as `const {} = ...;`",
        constant_name, constant_name, constant_name);
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
    return fmt::format("The type '{}' has no member named '{}'", type_name, member_name);
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

ISSUE_MESSAGE_FNC(UnmetInterfaceRequirement)
{
    return _message;
}
ISSUE_MESSAGE_FNC(DuplicateFunctionSignature)
{
    return _message;
}

ISSUE_MESSAGE_FNC(NoMatchingOverload)
{
    return _message;
}

ISSUE_MESSAGE_FNC(AmbiguousCall)
{
    return _message;
}

ISSUE_MESSAGE_FNC(UnsafePromotion)
{
    return fmt::format(
        "cannot form '{}' from a raw address outside an 'unsafe' block. A borrow is a trusted typed "
        "view: every access through it, here and in everything it is passed to, is optimized as '{}' "
        "- so establishing one over raw storage is a promise only you can make.",
        borrow_type, borrow_type);
}

std::vector<AST::IssueNote> AST::Issue::UnsafePromotion::notes() const
{
    return { IssueNote { NoteKind::t_help,
        "inside 'unsafe { }' you assert the address is non-null, aligned, holds a complete and valid "
        "value of that type, stays valid for as long as the borrow is used, and that reading it at "
        "that type is compatible with every other typed access to the same storage. It does not "
        "assert that the borrow is the only one" } };
}

ISSUE_MESSAGE_FNC(TopLevelCodeOutsideEntry)
{
    return fmt::format(
        "'{}' declares targets, so '{}' is shared by all of them and the code at its top level would "
        "never run. Only a target's entry file becomes a program.",
        module_name, file_name);
}

std::vector<AST::IssueNote> AST::Issue::TopLevelCodeOutsideEntry::notes() const
{
    return { IssueNote { NoteKind::t_help,
        "move it into the entry file of the target it belongs to, or into a function this one calls. "
        "Declarations are what a shared file is for, and every target already sees all of them" } };
}

ISSUE_MESSAGE_FNC(PrivateMember)
{
    return fmt::format(
        "'${}' is private to '{}' and cannot be reached from here. Its type keeps an invariant that "
        "depends on nothing outside it writing that field - go through a method of '{}' instead.",
        member_name, type_name, type_name);
}

ISSUE_MESSAGE_FNC(PrivateMethod)
{
    return fmt::format(
        "'{}' is private to '{}' and cannot be called from here. Only that type's own bodies reach it - "
        "if it is part of what '{}' offers, drop the 'private'.",
        member_name, type_name, type_name);
}

ISSUE_MESSAGE_FNC(InaccessibleDeclaration)
{
    return _message;
}

std::vector<AST::IssueLabel> AST::Issue::InaccessibleDeclaration::labels() const
{
    // the label only where the renderer can place it: a token another module owns resolves to no file
    // through the module this issue was built against, and the fallback would draw its line numbers
    // inside the file the diagnostic is already showing - see the kind's comment
    if (!declaration_token.has_value() || declared_in.module != code_ref.module) {
        return {};
    }

    return label_outside_primary(code_ref, declaration_token.value(), "declared here");
}

ISSUE_MESSAGE_FNC(ConflictingAccess)
{
    return _message;
}

std::vector<AST::IssueLabel> AST::Issue::ConflictingAccess::labels() const
{
    return label_outside_primary(code_ref, other_argument_token, "the other access is here");
}

std::vector<AST::IssueNote> AST::Issue::ConflictingAccess::notes() const
{
    if (remedy.empty()) {
        return {};
    }

    return { IssueNote { NoteKind::t_help, remedy } };
}
