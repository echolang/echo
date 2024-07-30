#include "AST/ASTMangler.h"

#include "AST/FunctionDeclNode.h"
#include "AST/ExprNode.h"

AST::mangled_id_t AST::mangle_function_name(const AST::FunctionDeclNode *func_decl)
{
    // @TODO move the mangling logic here
    return func_decl->decorated_func_name();
}

AST::mangled_id_t AST::mangle_function_name(const AST::FunctionCallExprNode *func_call)
{
    // @TODO move the mangling logic here
    return func_call->decorated_func_name();
}