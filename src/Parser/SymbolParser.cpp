#include "Parser/SymbolParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/NamespaceParser.h"
#include "Parser/TypeDeclParser.h"
#include "Parser/ExternParser.h"

#include "AST/ASTSymbol.h"
#include "AST/TypeDeclNode.h"

void Parser::parse_type_names(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    while (!cursor.is_done())
    {
        // `namespace a::b;` is a statement, not a block - it names the namespace the rest of the
        // file declares into, so this pass has to follow it to push a symbol into the right one
        if (cursor.is_type(Token::Type::t_namespace)) {
            parse_namespacedecl(payload);
            continue;
        }

        if (!starts_typedecl(cursor) || !cursor.peek_is_type(1, Token::Type::t_identifier)) {
            // token by token rather than skipping bodies whole, which is also how parse_symbols
            // walks - so a `struct` written inside a function body is reached by both
            cursor.skip();
            continue;
        }

        const AST::ComplexTypeKind kind = typedecl_kind(cursor);

        cursor.skip(); // the struct or class keyword

        auto name_token = cursor.current();
        cursor.skip();

        // find before create: push_symbol replaces the slot and frees what was there, so a second
        // declaration of the same name would leave the first node's Symbol dangling and hand
        // codegen two TypeDeclNodes for one type. parse_typedecl then reuses whichever node is here.
        // the duplicate is left symbol-less deliberately and reported nowhere here: both later passes
        // then find the *first* node and parse_typedecl reports the redeclaration at the duplicate's own
        // name token, which is also where the body-skip recovery lives. this pass has no such
        // recovery, and its detection is a strict subset of parse_typedecl's anyway
        auto *existing = payload.collector.namespaces.find_symbol(name_token.value(), *payload.context.current_namespace);
        if (existing != nullptr && existing->node.get_ptr<AST::TypeDeclNode>() != nullptr) {
            continue;
        }

        // the kind is settled here, at the only place the node is created. parse_typedecl reuses this
        // node in both later passes, so it never has to re-derive it from the keyword
        auto &type_node = payload.context.emplace_node<AST::TypeDeclNode>(name_token, kind);
        type_node.set_namespace(payload.context.current_namespace);

        payload.context.current_namespace->push_symbol(std::make_unique<AST::Symbol>(&type_node));
    }
}

void Parser::parse_symbols(Parser::Payload &payload)
{
    // this pass has no file root - the body pass builds that - but it does parse declarations, and
    // parse_varexpr takes the scope a *declaration* goes into as an argument while writing any
    // *statement* it reads into the ambient `context.scope()`, which would assert with nothing
    // pushed. a scratch scope so that path has somewhere to land; it guards parse_varexpr's ambient
    // write rather than expressing an intent to collect anything
    auto &declaration_scope = payload.context.emplace_node<AST::ScopeNode>();
    payload.context.push_scope(declaration_scope);

    while (!payload.cursor.is_done())
    {
        // functions do not become namespace symbols: a symbol slot holds one node per name, and a
        // name denotes an overload *set*. parse_funcdecl registers them in
        // Collector::functions instead, which is also why `struct Foo` and its constructor `Foo`
        // no longer fight over the same slot
        if (payload.cursor.is_type(Token::Type::t_function))
        {
            parse_funcdecl(payload);
        }
        else if (starts_typedecl(payload.cursor)) {
            // the name is already a symbol - parse_type_names pushed it, over every file, before
            // this pass started. that is what lets the declarations below name a type from any file
            parse_typedecl(payload);
        }
        else if (payload.cursor.is_type(Token::Type::t_extern))
        {
            // walked in this pass too, so the node it produces already knows it is extern. otherwise
            // a cross-module call would resolve to this node and mangle the Echo name while codegen
            // emitted the raw C symbol - an undefined symbol at link time. registration happens
            // inside parse_funcdecl, same as any other function
            parse_extern_block(payload);
        }
        else if (payload.cursor.is_type(Token::Type::t_namespace)) 
        {
            parse_namespacedecl(payload);
        }
        else {
            payload.cursor.skip();
        }
    }

    payload.context.pop_scope();
}