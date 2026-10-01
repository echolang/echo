#ifndef COMPILEREXCEPTION_H
#define COMPILEREXCEPTION_H

#pragma once

#include "AST/ASTIssue.h"

#include <exception>

namespace AST
{
    class File;
};

namespace Compiler
{
    class CompilerException : public std::exception
    {
    protected:
        std::string _message;
        const AST::File *_file = nullptr;

    public:
        CompilerException(const std::string &message) : _message(message) {}
        CompilerException(const std::string &message, const AST::File *file) : _message(message), _file(file) {}

        virtual const char *what() const noexcept override {
            return _message.c_str();
        }

        const AST::File *file() const {
            return _file;
        }

        // a located issue, or null. InternalCompilerException has a sentence and no span;
        // ASTCompilerException is the collector's own record. the driver asks this rather
        // than naming both siblings at every catch
        virtual const AST::IssueRecord *as_issue() const {
            return nullptr;
        }
    };

    class ASTCompilerException : public CompilerException
    {
        const AST::IssueRecord &_issue;

    public:
        ASTCompilerException(const AST::IssueRecord &issue) :
            CompilerException(issue.message(), issue.code_ref.file()),
            _issue(issue)
        {}

        const AST::IssueRecord &issue() const {
            return _issue;
        }

        virtual const AST::IssueRecord *as_issue() const override {
            return &_issue;
        }

        virtual const char *what() const noexcept override {
            return _message.c_str();
        }
    };

    class InternalCompilerException : public CompilerException
    {
    public:
        InternalCompilerException(const std::string &message) : CompilerException(message) {}
        InternalCompilerException(const std::string &message, const AST::File *file) : CompilerException(message, file) {}
    };
};

#endif
