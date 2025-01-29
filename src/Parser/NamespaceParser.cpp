#include "Parser/NamespaceParser.h"

bool is_part_of_namespace_token(Token::Type type)
{
    return type == Token::Type::t_identifier || type == Token::Type::t_namespace_sep;
}

size_t Parser::peek_past_namespace_prefix(Payload &payload, size_t offset)
{
    while (payload.cursor.is_type_sequence(offset, { Token::Type::t_identifier, Token::Type::t_namespace_sep })) {
        offset += 2;
    }
    return offset;
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
        payload.collect_unexpected_token(Token::Type::t_namespace);
        payload.cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // `namespace a;` names what the rest of the *file* declares into, so it is meaningless inside a
    // block - and worse than meaningless: the three parse passes walk blocks differently, so one of
    // them would follow this statement and the others would not, and a struct written after the block
    // would end up with two declaration nodes in two different namespaces
    //
    // being inside a block is exactly "the current namespace is lexical"
    if (payload.context.current_namespace != nullptr && payload.context.current_namespace->is_lexical()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(payload.cursor.current()),
            "A 'namespace' declaration cannot appear inside a body - it names what the rest of the file declares into.");
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
        payload.collect_unexpected_token(Token::Type::t_semicolon);
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