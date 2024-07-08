#include "AST/ASTSymbol.h"

#include "AST/FunctionDeclNode.h"

AST::Symbol::Symbol(AST::FunctionDeclNode *func) : 
    _type(SymbolType::t_function), 
    _name(func->name_token->value()), 
    node(AST::make_ref(func))
{
}