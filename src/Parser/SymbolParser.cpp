#include "Parser/SymbolParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/NamespaceParser.h"
#include "Parser/StructParser.h"
#include "Parser/ExternParser.h"

#include "AST/ASTSymbol.h"

void Parser::parse_symbols(Parser::Payload &payload)
{
    while (!payload.cursor.is_done())
    {
        // functions do not become namespace symbols: a symbol slot holds one node per name, and a
        // name denotes an overload *set*. parse_funcdecl registers them in
        // Collector::functions instead, which is also why `struct Foo` and its constructor `Foo`
        // no longer fight over the same slot
        if (payload.cursor.is_type(Token::Type::t_function))
        {
            parse_funcdecl(payload, true);
        }
        else if (payload.cursor.is_type(Token::Type::t_struct)) {
            auto structdecl = parse_struct(payload, true);
            if (structdecl) {
                payload.context.current_namespace->push_symbol(std::make_unique<AST::Symbol>(structdecl));
            }
        }
        else if (payload.cursor.is_type(Token::Type::t_extern))
        {
            // walked in the symbol pass too, so the node it produces already knows it is extern.
            // otherwise a cross-module call would resolve to this node and mangle the Echo name
            // while codegen emitted the raw C symbol - an undefined symbol at link time.
            // registration happens inside parse_funcdecl, same as any other function
            parse_extern_block(payload, true);
        }
        else if (payload.cursor.is_type(Token::Type::t_namespace)) 
        {
            parse_namespacedecl(payload);
        }
        else {
            payload.cursor.skip();
        }
    }
}