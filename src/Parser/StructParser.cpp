#include "Parser/StructParser.h"

#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/AssignNode.h"
#include "AST/ReturnNode.h"
#include "AST/VarRefNode.h"
#include "AST/VarNode.h"
#include "Parser/VarDeclParser.h"
#include "Parser/ScopeParser.h"
#include "Parser/TypeParser.h"

AST::StructDeclNode *Parser::parse_struct(Payload &payload, bool symbol_only)
{
    auto &cursor = payload.cursor;

    if (!cursor.is_type(Token::Type::t_struct)) {
        payload.collect_unexpected_token(Token::Type::t_struct);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the struct keyword
    cursor.skip();

    if (!cursor.is_type(Token::Type::t_identifier)) {
        payload.collect_unexpected_token(Token::Type::t_identifier);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // fetch the struct name and skip it
    auto name_token = cursor.current();
    cursor.skip();

    // optional generic type parameters: struct Foo<T, U> { ... }
    std::vector<ParsedTypeParam> parsed_type_params = parse_type_param_list(payload);

    // next token needs to be an open brace
    if (!cursor.is_type(Token::Type::t_open_brace)) {
        payload.collect_unexpected_token(Token::Type::t_open_brace);
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
    struct_node->set_namespace(payload.context.current_namespace);

    // declare the generic type parameters (idempotent across the symbol + full passes) and,
    // when the struct is generic, make them resolvable while parsing its property types.
    declare_type_parameters(payload, struct_node->complex_type(), parsed_type_params);
    const std::vector<AST::TypeParamDecl *> &type_parameters = struct_node->type_parameters();
    AST::TypeParamScope type_param_scope(payload.context, type_parameters);

    // the type a constructor returns: the plain struct for a non-generic one, or the
    // self-application Foo<T...> for a generic one. giving the constructor generic type
    // parameters + this return type lets the monomorphizer instantiate it alongside Foo<int>.
    AST::ValueType ctor_return_type = struct_node->value_type();
    if (struct_node->is_generic()) {
        auto *template_ct = ctor_return_type.get_complex_type();
        std::vector<AST::ValueType> self_args;
        for (const auto *param : template_ct->type_parameters) {
            self_args.push_back(AST::ValueType::make_type_param(param));
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
        if (starts_vardecl(payload)) {
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
                payload.collect_unexpected_token(Token::Type::t_open_paren);
                cursor.try_skip_to_next_statement();
                continue;
            }

            cursor.skip(); // skip "("

            // a constructor is named after its struct but is not *declared* at the struct's name
            // token, and the function registry keys a declaration on where it is written. sharing
            // the struct's token would make every constructor of a struct the same declaration
            auto ctor_name_token = payload.context.make_virtual_token(
                name_token.value(), Token::Type::t_identifier, name_token);

            auto &ctor_decl = payload.context.emplace_node<AST::FunctionDeclNode>(ctor_name_token);
            ctor_decl.ast_namespace = payload.context.current_namespace;

            // share the struct's parameter declarations rather than declaring its own: the ctor's
            // return type is the struct's self-application Foo<T>, so a substitution built from
            // this list has to bind the very same T that type mentions
            ctor_decl.type_parameters = type_parameters;

            auto &ctor_return = payload.context.emplace_node<AST::TypeNode>(ctor_return_type);
            ctor_decl.return_type = &ctor_return;

            // ctor argument + body scope
            auto &ctor_scope = payload.context.emplace_node<AST::ScopeNode>();

            // predeclare "$this" so member access works in body. it is seeded into the *body*
            // rather than this argument scope below, once the body node exists
            auto this_token = payload.context.make_virtual_token("$this", Token::Type::t_varname, name_token);
            auto this_vardecl = payload.context.emplace_nodep<AST::VarDeclNode>(this_token, &ctor_return);
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
                payload.collect_unexpected_token(Token::Type::t_close_paren);
                cursor.try_skip_to_next_statement();
                continue;
            }

            cursor.skip(); // skip ")"

            if (!cursor.is_type(Token::Type::t_open_brace)) {
                payload.collect_unexpected_token(Token::Type::t_open_brace);
                cursor.try_skip_to_next_statement();
                continue;
            }

            cursor.skip(); // skip "{"

            // the body opens with an implicit `Foo $this;`, exactly as the synthesized default
            // constructor below does. seeding it here rather than appending it after the body is
            // parsed is what makes it the first child, and the first child is the only position
            // that works: gen_scope allocas in child order, so a `$this` declared last has no
            // alloca yet when the statements above it read it, and CloneContext::rebind resolves
            // to the *original* for anything not yet cloned, so an instantiated generic ctor
            // would bind its `$this` reads to the template's declaration
            auto &ctor_body = payload.context.emplace_node<AST::ScopeNode>();
            ctor_body.add_vardecl(*this_vardecl);

            payload.context.push_scope(ctor_scope);

            {
                // same as a function body: a `return` inside the ctor fits the ctor's return type
                AST::ReturnTypeScope return_scope(payload.context, &ctor_return);
                ctor_decl.body = &parse_scope(payload, &ctor_body);
            }

            if (!cursor.is_type(Token::Type::t_close_brace)) {
                payload.collect_unexpected_token(Token::Type::t_close_brace);
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
            payload.collector.functions.register_function(
                payload.collector, payload.context.code_ref(name_token), &ctor_decl);
        }
        else if (cursor.is_type(Token::Type::t_close_brace)) {
            cursor.skip();
            break;
        }

        else {
            payload.collect_unexpected_token(Token::Type::t_unknown);

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

    // the field-wise constructor, synthesized for every struct: it takes one parameter per
    // property and initializes them in order.
    //
    // it is *not* suppressed merely because the user wrote a constructor of their own. Echo has
    // no other syntax for building a struct, so taking it away the moment a convenience
    // constructor appears would silently break every `Foo(...)` elsewhere in the program. It is
    // suppressed only when the user's own constructor already occupies the same signature, which
    // is checked once the parameter list below is built
    std::vector<AST::ValueType> default_ctor_params;
    for (const auto &prop : struct_node->properties()) {
        default_ctor_params.push_back(prop->type_node()->type);
    }

    if (payload.collector.functions.find_by_signature(
            name_token.value(), *payload.context.current_namespace, default_ctor_params)) {
        return struct_node;
    }

    auto default_ctor_name_token = payload.context.make_virtual_token(
        name_token.value(), Token::Type::t_identifier, name_token);

    auto &default_ctor = payload.context.emplace_node<AST::FunctionDeclNode>(default_ctor_name_token);
    default_ctor.ast_namespace = payload.context.current_namespace;

    // shares the struct's parameter declarations, same reason as the explicit constructor above
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
        AST::ExprNode *target = payload.context.emplace_nodep<AST::MemberAccessNode>(AST::make_ref(*this_ref), member_token);

        // a pointer property is *bound* here, not written through. a plain assignment to a
        // pointer means "store into the pointee", which for a field that has never been
        // seated writes through uninitialized memory - so the synthesized initializer spells
        // the re-seating form, `$this->prop:$ = $prop`, exactly as a user would
        // (book/concept/pointers_and_refs_v2.md, "Binding, writing, and re-seating")
        if (prop->has_type() && prop->type().is_pointer()) {
            target = payload.context.emplace_nodep<AST::PointerValueNode>(target, member_token);
        }

        // reference the matching constructor argument
        auto param_var = payload.context.emplace_nodep<AST::VarNode>(default_ctor.args[i]);
        auto param_ref = payload.context.emplace_nodep<AST::VarRefNode>(param_var);

        auto member_mut = payload.context.emplace_nodep<AST::AssignNode>(target, param_ref, member_token);

        // this is the one write a `const` property ever gets, so it is an initialization rather
        // than a mutation and the const checks in AST::TypeChecker have to let it through
        member_mut->is_initialization = true;

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
    payload.collector.functions.register_function(
        payload.collector, payload.context.code_ref(name_token), &default_ctor);

    return struct_node;
}