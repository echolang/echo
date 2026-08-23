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

// the previous declaration is a *label* - the renderer shows it in its own frame, quoting the line.
// see AST::to_diagnostic for how the token is resolved back to a file
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

ISSUE_MESSAGE_FNC(UnknownUse)
{
    return _message;
}

ISSUE_MESSAGE_FNC(DuplicateUse)
{
    return _message;
}

ISSUE_MESSAGE_FNC(AmbiguousUse)
{
    return _message;
}

ISSUE_MESSAGE_FNC(InvalidUse)
{
    return _message;
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

ISSUE_MESSAGE_FNC(UnknownStaticFunction)
{
    return fmt::format("The type '{}' has no static function named '{}'", type_name, function_name);
}

ISSUE_MESSAGE_FNC(CannotConstructType)
{
    return fmt::format("The type '{}' cannot be constructed", type_name);
}

std::vector<AST::IssueNote> AST::Issue::CannotConstructType::notes() const
{
    return { IssueNote { NoteKind::t_help,
        "only a struct or a class has constructors. T(...) names those after T is bound - "
        "a primitive, an interface, or a type parameter that never became one of those has none" } };
}

std::vector<AST::IssueNote> AST::Issue::UnknownStaticFunction::notes() const
{
    return { IssueNote { NoteKind::t_help,
        "a static function is declared 'static function' in the type's body and called on the type "
        "itself. an ordinary method is reached through a value instead, with '->'" } };
}

ISSUE_MESSAGE_FNC(UnknownStaticProperty)
{
    return fmt::format("The type '{}' has no static property named '{}'", type_name, property_name);
}

std::vector<AST::IssueNote> AST::Issue::UnknownStaticProperty::notes() const
{
    // the second note is the one a reader hits inside an initializer, and it is a *rule* rather than a
    // limitation worth apologising for: a static's initializer may name statics declared before it, and
    // nothing else - which is what makes a cycle between two of them unwritable rather than something
    // the compiler has to go looking for
    return {
        IssueNote { NoteKind::t_help,
            "a static property is declared 'static' in the type's body and read on the type itself. "
            "an ordinary property lives in each value instead, and is reached through one with '->'" },
        IssueNote { NoteKind::t_note,
            "inside a static's initializer only statics declared before it can be named - including "
            "the one being declared, which is not yet one of them" },
    };
}

ISSUE_MESSAGE_FNC(UnboundShorthandCall)
{
    return fmt::format(
        "'.{}(...)' takes its type from where its value goes, and nothing here says what that is",
        function_name);
}

std::vector<AST::IssueNote> AST::Issue::UnboundShorthandCall::notes() const
{
    return { IssueNote { NoteKind::t_help,
        "name the type - 'SomeType::" + function_name + "(...)' - or give the destination a declared "
        "type. a shorthand reads its owner from a return type, a declared variable's type, or the "
        "parameter it is passed to" } };
}

ISSUE_MESSAGE_FNC(AmbiguousShorthandCall)
{
    return _message;
}

std::vector<AST::IssueNote> AST::Issue::AmbiguousShorthandCall::notes() const
{
    return { IssueNote { NoteKind::t_help,
        "a shorthand has no type of its own, so it cannot be what tells these apart - and unlike an "
        "ordinary argument there is nothing to cast. name the type at the call instead" } };
}

ISSUE_MESSAGE_FNC(StaticOutsideType)
{
    return fmt::format("'{}' is not declared inside a type, so it cannot be static", function_name);
}

std::vector<AST::IssueNote> AST::Issue::StaticOutsideType::notes() const
{
    // worded for both positions this refuses - a declaration at file scope and one in a body - since
    // what is missing is the same thing in each: a type to do the owning
    return { IssueNote { NoteKind::t_help,
        "'static' says which type owns a declaration, and here there is none to own it. remove the "
        "modifier, or move the declaration into a type's body" } };
}

ISSUE_MESSAGE_FNC(ConstOnStaticFunction)
{
    return fmt::format("'{}' is static, so it has no receiver for 'const' to qualify", function_name);
}

std::vector<AST::IssueNote> AST::Issue::ConstOnStaticFunction::notes() const
{
    return { IssueNote { NoteKind::t_help,
        "'const function' says the method only reads the value it was called on. a static is called "
        "on the type and never on a value, so write 'const' on the parameters that are read-only "
        "instead" } };
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

ISSUE_MESSAGE_FNC(UnsafeFunctionPointerCast)
{
    return fmt::format(
        "cannot read a '{}' as a '{}' outside an 'unsafe' block. A C function pointer is a trusted "
        "typed callable: calling through it is the signature you wrote, so establishing one over a "
        "raw word - or extracting the word - is a promise only you can make.",
        from_type, to_type);
}

std::vector<AST::IssueNote> AST::Issue::UnsafeFunctionPointerCast::notes() const
{
    return { IssueNote { NoteKind::t_help,
        "inside 'unsafe { }' you assert the word is the address of a function with that signature, "
        "or that you are extracting that address to store it. The compiler does not check either. "
        "'&name' is the safe producer" } };
}

ISSUE_MESSAGE_FNC(AssumeRequiresUnsafe)
{
    return _message;
}

std::vector<AST::IssueNote> AST::Issue::AssumeRequiresUnsafe::notes() const
{
    return { IssueNote { NoteKind::t_help, "write it inside an 'unsafe' block" } };
}

ISSUE_MESSAGE_FNC(DuplicateTestName)
{
    return fmt::format("This file already declares a test called '{}'.", test_name);
}

std::vector<AST::IssueNote> AST::Issue::DuplicateTestName::notes() const
{
    return { IssueNote { NoteKind::t_help,
        "a test's name has to be unique within its own file and nowhere wider, so another file of this "
        "module may well have one of the same name" } };
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
    if (!declaration_token.has_value()) {
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

namespace
{
    std::vector<AST::IssueNote> help_if(const std::string &remedy)
    {
        if (remedy.empty()) {
            return {};
        }

        return { AST::IssueNote { AST::NoteKind::t_help, remedy } };
    }
};

ISSUE_MESSAGE_FNC(AddressOfTemporary) { return _message; }

ISSUE_MESSAGE_FNC(TemporaryMember) { return _message; }
std::vector<AST::IssueNote> AST::Issue::TemporaryMember::notes() const { return help_if(remedy); }

ISSUE_MESSAGE_FNC(AssignToNonPlace) { return _message; }
ISSUE_MESSAGE_FNC(UseAfterMove) { return _message; }
ISSUE_MESSAGE_FNC(ConditionalMove) { return _message; }
ISSUE_MESSAGE_FNC(PartialMove) { return _message; }
ISSUE_MESSAGE_FNC(MoveOfTemporary) { return _message; }
ISSUE_MESSAGE_FNC(MoveRequired) { return _message; }

ISSUE_MESSAGE_FNC(CannotCopy) { return _message; }
std::vector<AST::IssueNote> AST::Issue::CannotCopy::notes() const { return help_if(remedy); }

ISSUE_MESSAGE_FNC(NotIterable) { return _message; }
ISSUE_MESSAGE_FNC(ForeachBinding) { return _message; }
ISSUE_MESSAGE_FNC(ForeachKey) { return _message; }
ISSUE_MESSAGE_FNC(ForeachConstBorrow) { return _message; }

ISSUE_MESSAGE_FNC(BodylessFunction) { return _message; }
ISSUE_MESSAGE_FNC(ExternHasBody) { return _message; }
ISSUE_MESSAGE_FNC(ExternGeneric) { return _message; }
ISSUE_MESSAGE_FNC(DestructorHasParameters) { return _message; }
ISSUE_MESSAGE_FNC(DestructorHasReturnType) { return _message; }
ISSUE_MESSAGE_FNC(DuplicateDestructor) { return _message; }
ISSUE_MESSAGE_FNC(InvalidImplicitConversion) { return _message; }
ISSUE_MESSAGE_FNC(InvalidAttributeValue) { return _message; }

ISSUE_MESSAGE_FNC(UnknownManifestAttribute) { return _message; }
ISSUE_MESSAGE_FNC(ReservedManifestAttribute) { return _message; }
ISSUE_MESSAGE_FNC(RepeatedManifestAttribute) { return _message; }
ISSUE_MESSAGE_FNC(MissingModuleAttribute) { return _message; }
ISSUE_MESSAGE_FNC(UnusableModuleName) { return _message; }
ISSUE_MESSAGE_FNC(InvalidManifestScope) { return _message; }
ISSUE_MESSAGE_FNC(EmptySourcePattern) { return _message; }
ISSUE_MESSAGE_FNC(UnresolvableDependency) { return _message; }

ISSUE_MESSAGE_FNC(PackageNotVendored) { return _message; }

std::vector<AST::IssueNote> AST::Issue::PackageNotVendored::notes() const
{
    return { IssueNote { NoteKind::t_note, "run `epm install`" } };
}

ISSUE_MESSAGE_FNC(DuplicateModuleName) { return _message; }
ISSUE_MESSAGE_FNC(ModuleDependencyCycle) { return _message; }
ISSUE_MESSAGE_FNC(NoSuchManifest) { return _message; }
ISSUE_MESSAGE_FNC(UnusableTargetName) { return _message; }
ISSUE_MESSAGE_FNC(TargetEntryNotASource) { return _message; }
