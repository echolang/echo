#include "Parser/NamespaceParser.h"

bool is_part_of_namespace_token(Token::Type type)
{
    return type == Token::Type::t_identifier || type == Token::Type::t_namespace_sep;
}

AST::NamespaceNode *Parser::parse_namespace(Payload &payload)
{
    std::vector<std::string> ns_parts;

    auto start = payload.cursor.snapshot();

    while(
        !payload.cursor.is_done() && 
        payload.cursor.is_type_sequence(0, {Token::Type::t_identifier, Token::Type::t_namespace_sep})) 
    {
        ns_parts.emplace_back(payload.cursor.current().value());

        // skip the identifier
        payload.cursor.skip();

        // skip the namespace separator
        payload.cursor.skip();
    }

    // create the namespace node
    auto slice = payload.cursor.slice(start, payload.cursor.snapshot());
    auto &ns = payload.collector.namespaces.retrieve(ns_parts);
    auto &ns_node = payload.context.emplace_node<AST::NamespaceNode>(slice, &ns);

    return &ns_node;
}

AST::NamespaceDeclNode *Parser::parse_namespacedecl(Parser::Payload &payload)
{
    if (!payload.cursor.is_type(Token::Type::t_namespace)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_namespace, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the namespace keyword
    payload.cursor.skip();
    
    // collect the namespace tokens
    auto start = payload.cursor.snapshot();

    while (!payload.cursor.is_done() && is_part_of_namespace_token(payload.cursor.current().type())) {
        payload.cursor.skip();
    }

    // collect a slice
    auto ns_tokens = payload.cursor.slice(start, payload.cursor.snapshot());

    // we expect a semicolon
    if (!payload.cursor.is_type(Token::Type::t_semicolon)) {
        payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(payload.cursor.current()), Token::Type::t_semicolon, payload.cursor.current().type());
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the semicolon
    payload.cursor.skip();

    // create the namespace node
    // auto &funcall = payload.context.emplace_node<AST::FunctionCallExprNode>(funcname_token, args);
    std::vector<std::string> ns_parts;
    for (auto token : ns_tokens) {
        if (token.type() == Token::Type::t_identifier) {
            ns_parts.push_back(token.value());
        }
    }

    auto &ns = payload.collector.namespaces.retrieve(ns_parts);
    auto &ns_decl = payload.context.emplace_node<AST::NamespaceDeclNode>(ns_tokens, &ns);

    // set the current namespace in the context
    payload.context.current_namespace = &ns;

    return &ns_decl;
}