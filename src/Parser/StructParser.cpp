#include "Parser/StructParser.h"

#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/MemberMutNode.h"
#include "AST/ReturnNode.h"
#include "AST/VarRefNode.h"
#include "AST/VarNode.h"
#include "Parser/VarDeclParser.h"
#include "Parser/ScopeParser.h"
#include "Parser/TypeParser.h"

AST::StructDeclNode *Parser::parse_struct(Payload &payload, bool symbol_only)
{
    auto &cursor = payload.cursor;

    // RAII guard to clear the struct's type parameters from the context on exit,
    // mirroring the function-decl guard so property types resolve T -> t_generic.
    struct TypeParameterGuard {
        AST::Context &context;
        bool active = false;
        TypeParameterGuard(AST::Context &ctx) : context(ctx) {}
        void activate() { active = true; }
        ~TypeParameterGuard() {
            if (active) context.clear_type_parameters();
        }
    } guard(payload.context);

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

    // optional generic type parameters: struct Foo<T, U> { ... }
    std::vector<std::string> type_parameters = parse_type_param_list(payload);

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

    // record the generic type parameters (idempotent across the symbol + full passes) and,
    // when the struct is generic, make them resolvable while parsing its property types.
    struct_node->set_type_parameters(type_parameters);
    if (!type_parameters.empty()) {
        payload.context.set_type_parameters(type_parameters);
        guard.activate();
    }

    // the type a constructor returns: the plain struct for a non-generic one, or the
    // self-application Foo<T...> for a generic one. giving the constructor generic type
    // parameters + this return type lets the monomorphizer instantiate it alongside Foo<int>.
    AST::ValueType ctor_return_type = struct_node->value_type();
    if (struct_node->is_generic()) {
        auto *template_ct = ctor_return_type.get_complex_type();
        std::vector<AST::ValueType> self_args;
        for (size_t i = 0; i < template_ct->type_parameters.size(); i++) {
            self_args.push_back(AST::ValueType::make_type_param(i));
        }
        ctor_return_type = AST::ValueType::make_struct(
            payload.collector.type_registry.get_or_create_instantiation(template_ct, self_args));
    }

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
            auto var = parse_varexpr(payload, &structscope);

            // append the var as a property of the struct
            if (var) {
                struct_node->add_property(var);
            }
        }
        else if (cursor.is_type(Token::Type::t_identifier) && cursor.current().value() == "constructor") {
            cursor.skip(); // skip "constructor"

            // expect "("
            if (!cursor.is_type(Token::Type::t_open_paren)) {
                payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_open_paren, cursor.current().type());
                cursor.try_skip_to_next_statement();
                continue;
            }

            cursor.skip(); // skip "("

            auto &ctor_decl = payload.context.emplace_node<AST::FunctionDeclNode>(name_token);
            ctor_decl.ast_namespace = payload.context.current_namespace;
            ctor_decl.type_parameters = type_parameters;

            auto &ctor_return = payload.context.emplace_node<AST::TypeNode>(ctor_return_type);
            ctor_decl.return_type = &ctor_return;

            // ctor argument + body scope
            auto &ctor_scope = payload.context.emplace_node<AST::ScopeNode>();

            // predeclare "$this" so member access works in body
            auto this_token = payload.context.make_virtual_token("$this", Token::Type::t_varname, name_token);
            auto this_vardecl = payload.context.emplace_nodep<AST::VarDeclNode>(this_token, &ctor_return);
            ctor_scope.add_vardecl(*this_vardecl);
            auto this_var = payload.context.emplace_nodep<AST::VarNode>(this_vardecl);
            auto this_ref = payload.context.emplace_nodep<AST::VarRefNode>(this_var);

            while (!cursor.is_type(Token::Type::t_close_paren)) {
                if (cursor.is_done()) {
                    payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(name_token), Token::Type::t_close_paren, Token::Type::t_unknown);
                    cursor.try_skip_to_next_statement();
                    break;
                }

                auto arg_vardecl = parse_varexpr(payload, &ctor_scope);
                ctor_decl.args.push_back(arg_vardecl);
            }

            if (!cursor.is_type(Token::Type::t_close_paren)) {
                payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_close_paren, cursor.current().type());
                cursor.try_skip_to_next_statement();
                continue;
            }

            cursor.skip(); // skip ")"

            if (!cursor.is_type(Token::Type::t_open_brace)) {
                payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_open_brace, cursor.current().type());
                cursor.try_skip_to_next_statement();
                continue;
            }

            cursor.skip(); // skip "{"
            payload.context.push_scope(ctor_scope);
            ctor_decl.body = &parse_scope(payload);

            // ensure $this is part of the body scope tree
            ctor_decl.body->add_vardecl(*this_vardecl);

            if (!cursor.is_type(Token::Type::t_close_brace)) {
                payload.collector.collect_issue<AST::Issue::UnexpectedToken>(payload.context.code_ref(cursor.current()), Token::Type::t_close_brace, cursor.current().type());
                cursor.try_skip_to_next_statement();
            } else {
                cursor.skip(); // skip "}"
            }

            payload.context.pop_scope();

            // append implicit "return $this" if user did not return
            bool has_return = false;
            for (auto &child : ctor_decl.body->children) {
                if (child.has_type<AST::ReturnNode>()) {
                    has_return = true;
                    break;
                }
            }
            if (!has_return) {
                auto ret_stmt = payload.context.emplace_nodep<AST::ReturnNode>(this_ref);
                ctor_decl.body->children.push_back(AST::make_ref(ret_stmt));
            }

            payload.context.scope().add_funcdecl(ctor_decl);
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
    // this constructor will take parameters for each property and initialize them
    auto &default_ctor = payload.context.emplace_node<AST::FunctionDeclNode>(name_token);
    default_ctor.ast_namespace = payload.context.current_namespace;
    default_ctor.type_parameters = type_parameters;

    // create a type node for the return type
    auto &type_node = payload.context.emplace_node<AST::TypeNode>(ctor_return_type);
    default_ctor.return_type = &type_node;

    // add parameters for each struct property
    for (const auto &prop : struct_node->properties()) {
        // create a parameter with the same type as the property
        auto param_token = payload.context.make_virtual_token(prop->name(), Token::Type::t_varname, name_token);
        auto param_type = payload.context.emplace_nodep<AST::TypeNode>(prop->type_node()->type);
        auto param_var = payload.context.emplace_nodep<AST::VarDeclNode>(param_token, param_type);
        default_ctor.args.push_back(param_var);
    }

    // the function body is struct that initializes the properties of the struct 
    // with there initalizer expressions
    auto &ctor_body = payload.context.emplace_node<AST::ScopeNode>();
    default_ctor.body = &ctor_body;

    // allocate "$this" 
    auto this_token = payload.context.make_virtual_token("$this", Token::Type::t_varname, name_token);
    auto this_vardecl = payload.context.emplace_nodep<AST::VarDeclNode>(this_token, &type_node);
    default_ctor.body->add_vardecl(*this_vardecl);

    // create a var of the declaration
    auto this_var = payload.context.emplace_nodep<AST::VarNode>(this_vardecl);
    auto this_ref = payload.context.emplace_nodep<AST::VarRefNode>(this_var);

    // for each property we create an assignment statement in the constructor body
    for (size_t i = 0; i < struct_node->properties().size(); i++) {
        auto *prop = struct_node->properties()[i];

        // create $this->prop access
        auto member_token = payload.context.make_virtual_token(prop->name(), Token::Type::t_identifier, prop->token_varname);
        auto member_access = payload.context.emplace_nodep<AST::MemberAccessNode>(AST::make_ref(*this_ref), member_token);

        // reference the matching constructor argument
        auto param_var = payload.context.emplace_nodep<AST::VarNode>(default_ctor.args[i]);
        auto param_ref = payload.context.emplace_nodep<AST::VarRefNode>(param_var);

        auto member_mut = payload.context.emplace_nodep<AST::MemberMutNode>(member_access, param_ref);
        default_ctor.body->children.push_back(AST::make_ref(member_mut));
    }

    // return the initialized struct instance
    auto return_stmt = payload.context.emplace_nodep<AST::ReturnNode>(this_ref);
    default_ctor.body->children.push_back(AST::make_ref(return_stmt));

    // auto return_stmt = payload.context.emplace_nodep<AST::ReturnNode>(
    //     payload.context.emplace_nodep<AST::VarRefExprNode>(&this_ref)
    // );
    // default_ctor.body->children.push_back(AST::make_ref(return_stmt));

    // append the contstructor to the current context as a function
    payload.context.scope().add_funcdecl(default_ctor);

    return struct_node;
}