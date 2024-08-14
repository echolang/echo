#include "AST/ASTSymbol.h"

#include "AST/FunctionDeclNode.h"
#include "AST/StructNode.h"

AST::Symbol::Symbol(AST::FunctionDeclNode *func) : 
    _type(SymbolType::t_function), 
    _name(func->name_token->value()), 
    node(AST::make_ref(func))
{
}

AST::Symbol::Symbol(StructDeclNode *strct) :
    _type(SymbolType::t_struct),
    _name(strct->struct_name()),
    node(AST::make_ref(strct))
{
}