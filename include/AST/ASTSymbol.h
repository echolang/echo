#ifndef ASTSYMBOL_H
#define ASTSYMBOL_H

#pragma once

#include <string>
#include "AST/ASTNodeReference.h"

namespace AST
{  
    class FunctionDeclNode;
    class TypeDeclNode;

    // what a namespace symbol slot names. a slot holds one node per name, so this is the
    // function-or-type distinction and nothing finer: whether a type is a `struct` or a `class` is
    // ComplexType::kind's answer, and every consumer here dereferences the node to ask it anyway
    enum class SymbolType
    {
        t_function,
        t_type
    };

    class Symbol
    {
    public:

        NodeReference node;

        Symbol(SymbolType type, std::string name, NodeReference node) :
            node(node), _type(type), _name(name) 
        {}

        Symbol(FunctionDeclNode *func);
        Symbol(TypeDeclNode *type_decl);

        ~Symbol() {};

        SymbolType type() const { 
            return _type; 
        }

        std::string name() const { 
            return _name; 
        }

    private:

        SymbolType _type;
        std::string _name;

    };
};

#endif