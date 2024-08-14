#include "Parser/StructParser.h"

#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/ReturnNode.h"
#include "Parser/VarDeclParser.h"

AST::StructDeclNode *Parser::parse_struct(Payload &payload, bool symbol_only)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type(Token::Type::t_struct)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_struct, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the struct keyword
    cursor.skip();

    if (!cursor.is_type(Token::Type::t_identifier)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_identifier, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // fetch the struct name and skip it
    auto name_token = cursor.current();
    cursor.skip();

    // next token needs to be an open brace
    if (!cursor.is_type(Token::Type::t_open_brace)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_open_brace, cursor.current().type());
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    cursor.skip(); // skip the open brace

    // try to find the predeclared struct symbol
    auto structsymbol = payload.collector.namespaces.find_symbol(name_token.value(), *payload.context.current_namespace);
    AST::StructDeclNode *struct_node = nullptr;

    // we found a name matching symbol
    if (structsymbol) {
        auto symboldecl = structsymbol->node.get_ptr<AST::StructDeclNode>();
        if (symboldecl) {
            struct_node = symboldecl;
        }
    }

    // still no struct we create it
    if (!struct_node) {
        struct_node = &payload.context.emplace_node<AST::StructDeclNode>(name_token);
    }

    // create the struct node
    struct_node->ast_namespace = payload.context.current_namespace;

    // if the only thing we care for is the symbol we can stop here
    if (symbol_only) {
        // because we are already inside of the struct we can skip the the closing of the scope
        cursor.skip_till_end_of_scope();

        // return the struct
        return struct_node;
    }

    // add the struct to the current namespace 
    // this is so that we could declare recursive structs
    payload.context.scope().add_structdecl(*struct_node);

    // create an empty base scope for the function and the arguments to sit in
    auto &structscope = payload.context.emplace_node<AST::ScopeNode>();

    while (!cursor.is_done())
    {
        if (
            cursor.is_type(Token::Type::t_const) || // const keyword always starts a vardecl
            cursor.is_type(Token::Type::t_ptr) || // ptr keyword also indicates a vardecl
            cursor.is_type_sequence(0, { Token::Type::t_varname, Token::Type::t_assign }) ||
            cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_varname, Token::Type::t_assign }) || 
            cursor.is_type_sequence(0, { Token::Type::t_identifier, Token::Type::t_varname, Token::Type::t_semicolon })
        ) {
            auto var = parse_vardecl(payload, &structscope);

            // append the var as a property of the struct
            if (var) {
                struct_node->properties.push_back(var);
            }
        }
        else if (cursor.is_type(Token::Type::t_close_brace)) {
            cursor.skip();
            break;
        }

        else {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_unknown, cursor.current().type());

            // when we encounter an unexpected token, we skip until we find a semicolon or a brace
            // in the hopes that there is    simply a typo in the code or something minor that we can recover from
            // we might have to skip till the end of the scope otherwise..
            cursor.skip(); // always skip the token causing the issue
            cursor.skip_until({ Token::Type::t_semicolon, Token::Type::t_open_brace, Token::Type::t_close_brace });

            // if we find a semicolon or a close brace, we skip it in the hopes that afterwards we can continue parsing
            if (cursor.is_type({ Token::Type::t_semicolon, Token::Type::t_close_brace })) {
                cursor.skip();
            }
        }
    }

    // we also generate an implict constructor for the struct
    // this constructor will initialize all the properties of the struct with there inintializer expressions
    auto &default_ctor = payload.context.emplace_node<AST::FunctionDeclNode>(name_token);

    // create a type node
    auto &type_node = payload.context.emplace_node<AST::TypeNode>(struct_node->value_type());

    // the function return type is the struct type
    default_ctor.return_type = &type_node;

    // the function body is struct that initializes the properties of the struct 
    // with there initalizer expressions
    auto &ctor_body = payload.context.emplace_node<AST::ScopeNode>();
    default_ctor.body = &ctor_body;

    // add a return statement to the function body
    auto return_stmt = payload.context.emplace_nodep<AST::ReturnNode>(nullptr);
    default_ctor.body->children.push_back(AST::make_ref(return_stmt));

    // append the contstructor to the current context as a function
    payload.context.scope().add_funcdecl(default_ctor);

    return struct_node;
}