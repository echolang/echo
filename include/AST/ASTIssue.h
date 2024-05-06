#ifndef ASTISSUE_H
#define ASTISSUE_H

#pragma once

#include "ASTCodeRef.h"

#define MAKE_ISSUE_DEF1(className, severity, arg1Type, arg1Name) \
class className : public IssueRecord { \
public: \
    arg1Type arg1Name; \
    className(const CodeRef &code_ref, arg1Type arg1Name) : IssueRecord(severity, code_ref), arg1Name(arg1Name) {}; \
    ~className() {}; \
    const std::string message() const override; \
};

#define MAKE_ISSUE_DEF2(className, severity, arg1Type, arg1Name, arg2Type, arg2Name) \
class className : public IssueRecord { \
public: \
    arg1Type arg1Name; \
    arg2Type arg2Name; \
    className(const CodeRef &code_ref, arg1Type arg1Name, arg2Type arg2Name) : IssueRecord(severity, code_ref), arg1Name(arg1Name), arg2Name(arg2Name) {}; \
    ~className() {}; \
    const std::string message() const override; \
};


namespace AST
{  
    class VarDeclNode;

    enum class IssueSeverity
    {
        Error,
        Warning,
        Info
    };

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
            switch (severity)
            {
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

        virtual const std::string message() const = 0;
    };

    namespace Issue
    {
        MAKE_ISSUE_DEF1(GenericError, IssueSeverity::Error, const std::string&, _message);
        MAKE_ISSUE_DEF1(GenericWarning, IssueSeverity::Warning, const std::string&, _message);
        MAKE_ISSUE_DEF1(GenericInfo, IssueSeverity::Info, const std::string&, _message);

        MAKE_ISSUE_DEF2(UnexpectedToken, IssueSeverity::Error, Token::Type, expected, Token::Type, actual);
        MAKE_ISSUE_DEF1(VariableRedeclaration, IssueSeverity::Error, const VarDeclNode *, previous_declaration);
    }

};
#endif