#include "Parser/SymbolParser.h"
#include "Parser/FuncDeclParser.h"

#include "AST/ASTSymbol.h"

void Parser::parse_symbols(Parser::Payload &payload)
{
    while (!payload.cursor.is_done())
    {
        if (payload.cursor.is_type(Token::Type::t_function))
        {
            auto funcdecl = parse_funcdecl(payload, true);
            payload.context.current_namespace.push_symbol(std::make_unique<AST::Symbol>(funcdecl));
        }
        else if (payload.cursor.is_type(Token::Type::t_namespace)) 
        {
            
        }
        else {
            payload.cursor.skip();
        }
    }
}