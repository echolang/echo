#ifndef COMPILEREXCEPTION_H
#define COMPILEREXCEPTION_H

#pragma once

#include "AST/ASTIssue.h"

namespace AST
{
    class File;
}

namespace Compiler
{
    class CompilerException : public std::exception
    {
    protected:
        std::string _message;
        const AST::File *_file = nullptr;

    public:
        CompilerException(const std::string& message) : _message(message) {}
        CompilerException(const std::string& message, const AST::File *file) : _message(message), _file(file) {}

        virtual const char* what() const noexcept override {
            return _message.c_str();
        }

        const AST::File* file() const {
            return _file;
        }
    };

    class ASTCompilerException : public CompilerException
    {
        const AST::IssueRecord &_issue;

    public:
        ASTCompilerException(const AST::IssueRecord& issue) : 
            CompilerException(issue.message(), issue.code_ref.file),
            _issue(issue)
        {}

        const AST::IssueRecord& issue() const {
            return _issue;
        }

        virtual const char* what() const noexcept override {   
            return _message.c_str();
        }
    };

    class InternalCompilerException : public CompilerException
    {
    public:
        InternalCompilerException(const std::string& message) : CompilerException(message) {}
        InternalCompilerException(const std::string& message, const AST::File *file) : CompilerException(message, file) {}
    };
}

#endif