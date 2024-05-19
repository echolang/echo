#ifndef COMPILEREXCEPTION_H
#define COMPILEREXCEPTION_H

#pragma once

#include "AST/ASTIssue.h"

namespace Compiler
{
    class CompilerException : public std::exception
    {
        const AST::IssueRecord &_issue;

    public:
        CompilerException(const AST::IssueRecord& issue) : _issue(issue) {}

        const AST::IssueRecord& issue() const {
            return _issue;
        }

        virtual const char* what() const noexcept override
        {
            return _issue.message().c_str();
        }
    };
}

#endif