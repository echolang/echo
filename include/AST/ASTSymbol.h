#ifndef ASTSYMBOL_H
#define ASTSYMBOL_H

#pragma once

#include <string>
#include "AST/ASTNodeReference.h"

namespace AST
{  
    class FunctionDeclNode;
    class TypeDeclNode;
    class ConstDeclNode;

    // what a namespace symbol slot names. a slot holds one node per name, so this is the
    // function-or-type-or-constant distinction and nothing finer: whether a type is a `struct` or a `class`
    // is ComplexType::kind's answer, and every consumer here dereferences the node to ask it anyway
    //
    // a *constant* shares the slot with a type rather than getting a store of its own, which is what makes
    // `const foo = 1;` beside `struct foo` a reportable collision instead of two things answering to one
    // name. Note that Namespace::push_symbol replaces what it finds, so the check has to happen first
    enum class SymbolType
    {
        t_function,
        t_type,
        t_constant
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
        Symbol(ConstDeclNode *const_decl);

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