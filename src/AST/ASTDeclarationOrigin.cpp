#include "AST/ASTDeclarationOrigin.h"

#include "AST/ASTContext.h"

AST::DeclarationOrigin AST::origin_at(const AST::Context &context)
{
    return AST::DeclarationOrigin { &context.module, context.file.file };
}
