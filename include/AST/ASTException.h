#ifndef ASTEXCEPTION_H
#define ASTEXCEPTION_H

#pragma once

#include <exception>
#include <string>

#define MAKE_AST_LOGIC_EXCEPTION(name, message) \
    struct name : public AST::ASTLogicException { \
        const char *what() const noexcept override { \
            return message; \
        } \
    };

namespace AST
{
    struct ASTException : public std::exception {};
    struct ASTLogicException : public ASTException {};

    struct GenericASTLogicException : public ASTLogicException
    {
        GenericASTLogicException(const std::string &message) : _message(message) {}

        const char *what() const noexcept override  {
            return _message.c_str();
        }
    private:
        std::string _message;
    };

    namespace LogicException
    {
        MAKE_AST_LOGIC_EXCEPTION(UnexpectedPrimitiveType, "Expected a complex type, but got a primitive type");
        MAKE_AST_LOGIC_EXCEPTION(UnexpectedComplexType, "Expected a primitive type, but got a complex type");
        MAKE_AST_LOGIC_EXCEPTION(UnresolableComplexType, "Complex type could not be resolved");
        MAKE_AST_LOGIC_EXCEPTION(InvalidMemberIndex, "Invalid member index");
        MAKE_AST_LOGIC_EXCEPTION(UnexpectedNodeReferenceType, "The referenced node is of an unexpected type");
    };
};

#endif
