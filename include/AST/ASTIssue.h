#ifndef ASTISSUE_H
#define ASTISSUE_H

#pragma once

#include "AST/ASTCodeRef.h"
#include "AST/ASTDeclarationOrigin.h"

#include <optional>
#include <vector>

// **the class name is the diagnostic code.** a code has to be stable, unique and spelled once, and the
// C++ name already is all three - so it is taken rather than invented, and there is no registry to keep in
// step with the classes. `AST::Issue::GenericError` overrides it back to nothing, because "unclassified"
// is what that kind means and a tool must not be told otherwise
#define ISSUE_CODE_OF(className) \
    std::optional<std::string> code() const override { return std::string(#className); }

// the trailing `...` is where a kind declares the optional extras it answers - a `labels()` or a
// `notes()` override, spelled as it would be in the class body. Empty for almost every kind, which is the
// point: the two that carry a second location say so in one line instead of leaving the macro behind
#define MAKE_ISSUE_DEF1(className, severity, arg1Type, arg1Name, ...) \
class className : public IssueRecord { \
public: \
    arg1Type arg1Name; \
    className(const CodeRef &code_ref, arg1Type arg1Name) : IssueRecord(severity, code_ref), arg1Name(arg1Name) {}; \
    ~className() {}; \
    ISSUE_CODE_OF(className) \
    std::string message() const override; \
    __VA_ARGS__ \
};

#define MAKE_ISSUE_DEF2(className, severity, arg1Type, arg1Name, arg2Type, arg2Name, ...) \
class className : public IssueRecord { \
public: \
    arg1Type arg1Name; \
    arg2Type arg2Name; \
    className(const CodeRef &code_ref, arg1Type arg1Name, arg2Type arg2Name) : IssueRecord(severity, code_ref), arg1Name(arg1Name), arg2Name(arg2Name) {}; \
    ~className() {}; \
    ISSUE_CODE_OF(className) \
    std::string message() const override; \
    __VA_ARGS__ \
};

#define MAKE_ISSUE_DEF3(className, severity, arg1Type, arg1Name, arg2Type, arg2Name, arg3Type, arg3Name, ...) \
class className : public IssueRecord { \
public: \
    arg1Type arg1Name; \
    arg2Type arg2Name; \
    arg3Type arg3Name; \
    className(const CodeRef &code_ref, arg1Type arg1Name, arg2Type arg2Name, arg3Type arg3Name) : IssueRecord(severity, code_ref), arg1Name(arg1Name), arg2Name(arg2Name), arg3Name(arg3Name) {}; \
    ~className() {}; \
    ISSUE_CODE_OF(className) \
    std::string message() const override; \
    __VA_ARGS__ \
};

// the same shape with no code, for a kind whose name is not a classification. **Not a variant of
// MAKE_ISSUE_DEF1 with a flag**: the whole difference is that this one has nothing to tell a tool, and a
// parameter reading `false` at three sites hides that behind punctuation
#define MAKE_UNCODED_ISSUE_DEF(className, severity) \
class className : public IssueRecord { \
public: \
    const std::string _message; \
    className(const CodeRef &code_ref, const std::string _message) : IssueRecord(severity, code_ref), _message(_message) {}; \
    ~className() {}; \
    std::string message() const override; \
};


namespace AST
{
    class VarDeclNode;
    enum class ValueTypePrimitive;
    class ValueType;

    enum class IssueSeverity
    {
        Error,
        Warning,
        Info
    };

    // a line under the message, after the source frame. `t_help` is a remedy the reader can act on,
    // `t_note` is a fact that explains the refusal - the split every diagnostic renderer since gcc has
    // made, and the one an editor uses to decide what to offer as a fix
    enum class NoteKind
    {
        t_note,
        t_help
    };

    struct IssueNote
    {
        NoteKind kind;
        std::string message;
    };

    // a *second* place the reader has to look at - the previous declaration, the other candidate, the
    // borrow that is still live. carried as a token slice rather than a CodeRef because the sites that
    // have one to give are holding a token: `AST::to_diagnostic` resolves which file it came from through
    // `Module::file_of`, which is a question the issue has no business answering
    struct IssueLabel
    {
        TokenSlice span;
        std::string message;
    };

    // **a label is a *second place to look at*, and a span on a line the primary frame already draws is
    // not one** - the renderer gives every label a frame of its own, so it would come out as the same
    // source line printed twice. `$a->extend($a)` and a declaration refused from the line below it are
    // both exactly that case.
    //
    // one function rather than the rule spelled in each `labels()` override, the answer being about the
    // renderer's one-frame-per-label behaviour and not about any one kind
    std::vector<IssueLabel> label_outside_primary(
        const CodeRef &code_ref,
        const TokenReference &at,
        std::string message
    );

    class IssueRecord
    {
    public:

        const IssueSeverity severity;
        const CodeRef code_ref;

        IssueRecord(IssueSeverity severity, const CodeRef &code_ref) :
            severity(severity),
            code_ref(code_ref)
        {}
        virtual ~IssueRecord() {};

        const std::string severity_string() const
        {
            switch (severity) {
            case IssueSeverity::Error:
                return "Error";
            case IssueSeverity::Warning:
                return "Warning";
            case IssueSeverity::Info:
                return "Info";
            default:
                return "Unknown";
            }
        }

        bool is_critical() const {
            return severity == IssueSeverity::Error;
        }

        virtual std::string message() const = 0;

        // **the four renderable extras, all optional.** a kind that overrides none of them renders exactly
        // as it always did - a message and a frame - which is why 148 GenericError sites needed no edit to
        // move onto the new renderer. They exist so that the information a kind already holds stops being
        // formatted into English: a second location becomes a label instead of "on line 4 column 3", and a
        // remedy becomes a `help` line instead of a third sentence.
        //
        // defaulted here rather than pure, deliberately: a diagnostic that says only what is wrong is a
        // complete diagnostic, and forcing every kind to answer would get 148 empty overrides
        virtual std::optional<std::string> code() const { return std::nullopt; }

        // the few words that go beside the underline, in the frame. Short - it shares a line with the
        // source - where `message()` is the sentence above it
        virtual std::string primary_label() const { return {}; }

        virtual std::vector<IssueNote> notes() const { return {}; }
        virtual std::vector<IssueLabel> labels() const { return {}; }
    };

    namespace Issue
    {
        // **the three that carry no code**, because their name is not a classification: 148 of the ~203
        // reporting sites raise a GenericError, and telling an editor that they are all one diagnostic
        // kind would be a fact it would act on. Absent says the true thing - this one has not been
        // classified yet
        MAKE_UNCODED_ISSUE_DEF(GenericError, IssueSeverity::Error);
        MAKE_UNCODED_ISSUE_DEF(GenericWarning, IssueSeverity::Warning);
        MAKE_UNCODED_ISSUE_DEF(GenericInfo, IssueSeverity::Info);

        MAKE_ISSUE_DEF2(UnexpectedToken, IssueSeverity::Error, Token::Type, expected, Token::Type, actual,
            std::string primary_label() const override;);
        MAKE_ISSUE_DEF1(VariableRedeclaration, IssueSeverity::Error, const VarDeclNode *, previous_declaration,
            std::vector<IssueLabel> labels() const override;);
        // a second declaration of a type name that is already declared in this namespace. carries a
        // name and the previous declaration's token rather than a TypeDeclNode *, so a `class` or
        // an `enum` can reuse the kind unchanged - hence the type-neutral name
        MAKE_ISSUE_DEF2(TypeRedeclaration, IssueSeverity::Error, const std::string, type_name, const TokenReference, previous_declaration_token,
            std::vector<IssueLabel> labels() const override;);
        MAKE_ISSUE_DEF1(UnknownVariable, IssueSeverity::Error, const std::string, variable_name);
        MAKE_ISSUE_DEF1(UnknownFunction, IssueSeverity::Error, const std::string, function_name);
        // a bare identifier in an operand position that names no constant. its own kind rather than a
        // GenericError because it is one of the two things a reader could have meant, and the message has to
        // offer the other: a value carries a `$`, so a missing one lands here rather than at UnknownVariable
        MAKE_ISSUE_DEF1(UnknownConstant, IssueSeverity::Error, const std::string, constant_name);
        // MAKE_ISSUE_DEF2(ValueTypeConflict, IssueSeverity::Error, const ValueType *, expected, ValueType *, actual);

        MAKE_ISSUE_DEF1(LossOfPrecision, IssueSeverity::Warning, const std::string, _message);
        MAKE_ISSUE_DEF1(InvalidTypeConversion, IssueSeverity::Error, const std::string, _message);
        // writing to storage the type says is read-only. not a conversion - the types match, the
        // permission does not - so it does not belong under InvalidTypeConversion
        MAKE_ISSUE_DEF1(ConstViolation, IssueSeverity::Error, const std::string, _message);
        MAKE_ISSUE_DEF1(IntegerOverflow, IssueSeverity::Error, const std::string, _message);
        MAKE_ISSUE_DEF1(IntegerUnderflow, IssueSeverity::Error, const std::string, _message);

        // semantic analysis diagnostics (recorded by the type-check pass, never thrown)
        MAKE_ISSUE_DEF2(UnknownMember, IssueSeverity::Error, const std::string, member_name, const std::string, type_name);
        MAKE_ISSUE_DEF1(ArgumentTypeMismatch, IssueSeverity::Error, const std::string, _message);
        MAKE_ISSUE_DEF1(UnresolvedTypeParameter, IssueSeverity::Error, const std::string, _message);
        MAKE_ISSUE_DEF1(UnsatisfiedTypeConstraint, IssueSeverity::Error, const std::string, _message);

        // a declared `: SomeInterface` whose requirements the type does not answer. its own kind rather
        // than a GenericError because it is the one diagnostic that makes a *declared* conformance mean
        // anything - every use site trusts the claim without re-deriving it, so this is where the claim
        // is paid for
        MAKE_ISSUE_DEF1(UnmetInterfaceRequirement, IssueSeverity::Error, const std::string, _message);

        // overload resolution. a name that is not declared at all stays UnknownFunction - these
        // three are the cases where candidates exist but none or several of them answer the call
        MAKE_ISSUE_DEF1(DuplicateFunctionSignature, IssueSeverity::Error, const std::string, _message);
        MAKE_ISSUE_DEF1(NoMatchingOverload, IssueSeverity::Error, const std::string, _message);
        MAKE_ISSUE_DEF1(AmbiguousCall, IssueSeverity::Error, const std::string, _message);

        // a `T&` formed from a raw address outside an `unsafe` block. its own kind because it is *the*
        // semantic boundary of the type model: a borrow's type is a contract every later access is
        // optimized against, and this is the one place a program can assert that contract over storage
        // the compiler cannot check
        MAKE_ISSUE_DEF1(UnsafePromotion, IssueSeverity::Error, const std::string, borrow_type,
            std::vector<IssueNote> notes() const override;);

        // a `private` property reached from outside the type that declared it. its own kind because it
        // is the diagnostic that makes an invariant enforceable rather than merely documented - every
        // aliasing conclusion `mem::buffer<T>` licenses rests on this refusal existing
        MAKE_ISSUE_DEF2(PrivateMember, IssueSeverity::Error, const std::string, member_name, const std::string, type_name);

        // a `private` **method** called from outside the type that declared it. a kind of its own rather
        // than PrivateMember's second reading, for two reasons that are the same reason: the advice
        // differs - "go through a method" is what PrivateMember says, and a method *is* one - and an
        // issue's message is part of the collector's dedup key, so one kind cannot carry two sentences
        MAKE_ISSUE_DEF2(PrivateMethod, IssueSeverity::Error, const std::string, member_name, const std::string, type_name);

        // a declaration named from outside the file or module it is reachable from. one kind for both
        // scopes, the sentence coming from AST::visibility_refusal - which is the rule, so a diagnostic
        // that worded it here would be a second answer to "what does this modifier mean"
        //
        // `declaration_token` is a *label*: the second place to look is where the declaration was written,
        // and the message already says which scope it named.
        //
        // **not drawn for a refusal across modules**, and that is not an omission. AST::span_of resolves a
        // label's file through `Module::file_of` on the module the *issue* was built against, which answers
        // null for a token another module owns - and then falls back to the file the diagnostic is already
        // drawing. A cross-module label would therefore point at the declaration's line numbers inside the
        // *caller's* file, which is worse than pointing nowhere.
        //
        // `declared_in` is here for exactly that: the module the token belongs to, against `code_ref`'s -
        // which is the module `span_of` will resolve it through. Asked here rather than by the four sites
        // that report this, none of which has any other use for the answer
        MAKE_ISSUE_DEF3(InaccessibleDeclaration, IssueSeverity::Error, const std::string, _message, const DeclarationOrigin, declared_in, const std::optional<TokenReference>, declaration_token,
            std::vector<IssueLabel> labels() const override;);

        // two arguments of one call name overlapping storage, and at least one of them is written
        // through. its own kind rather than a GenericError because it is the diagnostic that makes an
        // access effect mean anything - every optimisation the effect eventually licenses rests on
        // this refusal, so it is where the promise is paid for
        //
        // the second argument's token is a *label* and the remedy a *note*: the sentence says what is
        // wrong once, and the collector's dedup key is that sentence - so a second location folded
        // into the English would make two conflicts at one line read as one
        MAKE_ISSUE_DEF3(ConflictingAccess, IssueSeverity::Error, const std::string, _message, const TokenReference, other_argument_token, const std::string, remedy,
            std::vector<IssueLabel> labels() const override;
            std::vector<IssueNote> notes() const override;);

    };
};
#endif
