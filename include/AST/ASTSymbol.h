#ifndef ASTSYMBOL_H
#define ASTSYMBOL_H

#pragma once

#include "AST/ASTNodeReference.h"
#include "Token.h"

#include <optional>
#include <string>

namespace AST
{
    class FunctionDeclNode;
    class TypeDeclNode;
    class ConstDeclNode;
    class TypeAliasDeclNode;

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
        t_constant,
        t_alias
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
        Symbol(TypeAliasDeclNode *alias);

        ~Symbol() {};

        SymbolType type() const {
            return _type;
        }

        std::string name() const {
            return _name;
        }

        // a type or an alias of one
        bool names_type() const {
            return _type == SymbolType::t_type || _type == SymbolType::t_alias;
        }

        // the name token of the type, constant or alias this slot holds. nullopt for a function
        std::optional<TokenReference> declaration_token() const;

    private:

        SymbolType _type;
        std::string _name;

    };
};

#endif
