#ifndef ASTSYMBOL_H
#define ASTSYMBOL_H

#pragma once

#include <string>
#include "AST/ASTNodeReference.h"

namespace AST
{  
    class FunctionDeclNode;

    enum class SymbolType
    {
        t_function,
    };

    class Symbol
    {
    public:

        NodeReference node;

        Symbol(SymbolType type, std::string name, NodeReference node) :
            node(node), _type(type), _name(name) 
        {}

        Symbol(FunctionDeclNode *func);

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