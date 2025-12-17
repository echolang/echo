#include "AST/ASTSymbol.h"

#include "AST/ConstDeclNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/TypeDeclNode.h"

AST::Symbol::Symbol(AST::FunctionDeclNode *func) :
    _type(SymbolType::t_function),
    _name(func->name_token->value()),
    node(AST::make_ref(func))
{
}

AST::Symbol::Symbol(TypeDeclNode *type_decl) :
    _type(SymbolType::t_type),
    _name(type_decl->type_name()),
    node(AST::make_ref(type_decl))
{
}

AST::Symbol::Symbol(ConstDeclNode *const_decl) :
    _type(SymbolType::t_constant),
    _name(const_decl->name()),
    node(AST::make_ref(const_decl))
{
}
