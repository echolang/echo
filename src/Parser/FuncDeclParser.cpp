#include "Parser/FuncDeclParser.h"

#include "AST/VarDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/ASTBuiltin.h"

#include "Parser/TypeParser.h"
#include "Parser/ExprParser.h"
#include "Parser/VarDeclParser.h"
#include "Parser/ScopeParser.h"

#include <fmt/core.h>

// reads the single string value out of an attribute like `#[intrinsic: "llvm.sin"]`, reporting a
// located issue and answering nullopt when the attribute is malformed. shared by every attribute
// whose payload is one string, so they cannot diverge in how they validate
static std::optional<std::string> attribute_string_value(
    Parser::Payload &payload, AST::AttributeNode *attribute, const std::string &attribute_name)
{
    if (attribute->attribute_exprs.size() != 1) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(attribute->attribute_tokens),
            fmt::format("The '{}' attribute takes exactly one value.", attribute_name));
        return std::nullopt;
    }

    if (!attribute->attribute_exprs[0].has_type<AST::LiteralStringExprNode>()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(attribute->attribute_tokens),
            fmt::format("The '{}' attribute value must be a string.", attribute_name));
        return std::nullopt;
    }

    return attribute->attribute_exprs[0].get_ptr<AST::LiteralStringExprNode>()->get_string_value();
}

void Parser::push_receiver_param(
    Parser::Payload &payload,
    AST::FunctionDeclNode &decl,
    AST::ScopeNode &into,
    AST::TypeNode *self_type,
    const TokenReference &at)
{
    auto this_token = payload.context.make_virtual_token("$this", Token::Type::t_varname, at);
    auto *this_vardecl = payload.context.emplace_nodep<AST::VarDeclNode>(this_token, self_type);

    // ahead of everything the caller wrote. FunctionDeclNode::clone clones args before the body
    // precisely so the body's references rebind to the cloned declaration
    decl.args.push_back(this_vardecl);

    // declared in the argument scope, not in the body, so `$this` resolves the same way every other
    // parameter does. the argument scope's children are never emitted - the body is a separate child
    // scope and codegen allocas from `args` - so this costs no second slot
    into.add_vardecl(*this_vardecl);
}

bool Parser::parse_parameter_list(
    Parser::Payload &payload,
    AST::FunctionDeclNode &decl,
    AST::ScopeNode &into,
    const TokenReference &report_at)
{
    auto &cursor = payload.cursor;

    while (!cursor.is_type(Token::Type::t_close_paren)) {
        if (cursor.is_done()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                payload.context.code_ref(report_at), Token::Type::t_close_paren, Token::Type::t_unknown);
            cursor.try_skip_to_next_statement();
            return false;
        }

        // `mv Buffer $items` - this parameter takes ownership of its argument. read here rather than
        // in parse_varexpr because it is a property of a *parameter*, not of a declaration: there is
        // no `mv` on a local, and a local's owner is the scope it is declared in either way
        const bool takes_ownership = cursor.is_type(Token::Type::t_mv);
        if (takes_ownership) {
            cursor.skip();
        }

        auto *param = parse_varexpr(payload, &into);

        if (param != nullptr) {
            param->takes_ownership = takes_ownership;
        }

        decl.args.push_back(param);
    }

    // skip the close parenthesis
    cursor.skip();

    return true;
}

AST::FunctionDeclNode * Parser::parse_funcdecl(Parser::Payload &payload, Parser::FuncDeclKind kind)
{
    auto &cursor = payload.cursor;

    // the declaration pass stops once the signature is registered; the body pass carries on into the
    // body. read off the payload rather than taken as an argument, so no caller in between has to
    // know which pass it is forwarding
    const bool symbol_only = payload.pass == Pass::t_declarations;

    if (!cursor.is_type(Token::Type::t_function)) {
        payload.collect_unexpected_token(Token::Type::t_function);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the function keyword
    cursor.skip();

    // next token should be an identifier aka the function name
    if (!cursor.is_type(Token::Type::t_identifier)) {
        payload.collect_unexpected_token(Token::Type::t_identifier);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // inside an extern block the first identifier is the C symbol, and an optional `as` renames
    // it for Echo callers: `function malloc as alloc_bytes(...)`. without the rename the symbol
    // and the Echo name are the same.
    //
    // resolved before the name token is captured below, because the Echo name is what the symbol
    // table, the namespace and every call site use - only extern_symbol reaches the linker
    std::optional<std::string> extern_symbol;
    if (kind == FuncDeclKind::t_extern) {
        extern_symbol = cursor.current().value();

        if (cursor.peek_is_type(1, Token::Type::t_as)) {
            cursor.skip(); // the C symbol
            cursor.skip(); // the `as` keyword

            if (!cursor.is_type(Token::Type::t_identifier)) {
                payload.collect_unexpected_token(Token::Type::t_identifier);
                cursor.try_skip_to_next_statement();
                return nullptr;
            }
        }
    }

    // fetch the function name and skip it
    auto nametoken = cursor.current();
    cursor.skip();

    // check for optional generic type parameters: <T, U, ...>
    std::vector<ParsedTypeParam> parsed_type_params = parse_type_param_list(payload);

    // next token needs to be an open parenthesis
    if (!cursor.is_type(Token::Type::t_open_paren)) {
        payload.collect_unexpected_token(Token::Type::t_open_paren);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // a module's passes must land on *one* node per declaration, or the body pass's body is attached
    // to a node no call resolves to. they are matched on where the declaration is written rather than
    // on its name: the name identifies an overload *set*, and this decision has to be made here, at
    // the name token, before the parameter list that would tell the overloads apart has been parsed
    AST::FunctionDeclNode *funcdecl = payload.collector.functions.find_by_declaration_site(nametoken);

    if (funcdecl != nullptr) {
        // the arguments are rebuilt against this pass's context, so drop the previous pass's
        funcdecl->args.clear();
    } else {
        funcdecl = &payload.context.emplace_node<AST::FunctionDeclNode>(nametoken);
    }

    // set the namespace of the function
    funcdecl->ast_namespace = payload.context.current_namespace;

    // a `function` written inside a struct body is a method: the enclosing struct arrived on the
    // context, and it is the only thing that distinguishes this from a free function
    AST::TypeDeclNode *owner_struct = payload.context.self_struct_ptr;
    AST::TypeNode *self_type = payload.context.self_type_ptr;
    const bool is_method = owner_struct != nullptr && self_type != nullptr;

    // a method of a generic struct carries the owner's parameters ahead of its own, so that one
    // TypeSubstitution binds both: the owner's T from the receiver argument, its own U from the rest.
    // declare_type_parameters owns that shape - what is shared, what is re-declared, and where the
    // split falls
    std::vector<AST::TypeParamDecl *> inherited_params;
    if (is_method && owner_struct->is_generic()) {
        inherited_params = owner_struct->type_parameters();
    }

    // declare the type parameters, then make them resolvable while the signature and body are
    // parsed. the scope is pushed unconditionally, even when empty, so that a non-generic
    // function nested in a generic owner leaves the owner's parameters visible
    declare_type_parameters(payload, *funcdecl, parsed_type_params, inherited_params);

    AST::TypeParamScope type_param_scope(payload.context, funcdecl->type_parameters);

    // skip the open parenthesis
    cursor.skip();

    // create an empty base scope for the function and the arguments to sit in
    auto &funcscope = payload.context.emplace_node<AST::ScopeNode>();

    // the receiver is a real parameter, ahead of everything the caller wrote - see
    // Parser::push_receiver_param, shared with the destructor arm
    if (is_method) {
        funcdecl->member_kind = AST::MemberKind::t_method;
        push_receiver_param(payload, *funcdecl, funcscope, self_type, nametoken);
    }

    // parse the function arguments
    if (!parse_parameter_list(payload, *funcdecl, funcscope, nametoken)) {
        return nullptr;
    }

    // next token should be ":" for the return type
    if (!cursor.is_type(Token::Type::t_colon)) {
        payload.collect_unexpected_token(Token::Type::t_colon);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the colon
    cursor.skip();

    // parse the return type
    if (!can_parse_type(payload)) {
        payload.collect_unexpected_token(Token::Type::t_identifier);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    funcdecl->return_type = parse_type(payload);

    // the signature is complete, so this is the earliest point the declaration can join its
    // overload set. registering in *both* passes is intentional and cheap: the symbol pass makes
    // the declaration visible to calls written above it and in other files, and the full pass
    // finds its own declaration site already present and returns the same handle
    if (is_method) {
        // a method joins its owner's method table instead, so it is reachable through a receiver
        // and not as a free function of the enclosing namespace. owner_type is what tells the
        // mangler and every diagnostic that the first parameter is not one the user wrote
        funcdecl->owner_type = &owner_struct->complex_type();

        payload.collector.functions.register_member_function(
            payload.collector, payload.context.code_ref(nametoken), funcdecl, owner_struct->complex_type());
    }
    else {
        payload.collector.functions.register_function(
            payload.collector, payload.context.code_ref(nametoken), funcdecl);
    }

    // an extern declaration ends here, in both parser passes, so this is the single place that
    // owns its tail. doing it before the symbol_only return below is deliberate: the symbol pass
    // and the full pass build separate nodes for the same declaration, a cross-module call
    // resolves through the symbol pass's node while codegen emits from the full pass's, and the
    // two only agree because they mangle identically. if just one of them knew it was extern, the
    // call would reference `_mem_alloc_bytesZZ...` while the definition was named `malloc`
    if (kind == FuncDeclKind::t_extern) {
        // a raw symbol has exactly one definition, so it cannot have one body per instantiation.
        // this is why `mem::alloc<T>` is Echo code over a concrete `alloc_bytes` rather than an
        // extern of its own
        if (funcdecl->is_generic()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(nametoken),
                "An extern function cannot be generic - a single C symbol has no per-instantiation body");
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        // the body lives in another object file. a body here would be compiled under the raw
        // symbol and collide with the real definition at link time
        if (!cursor.is_type(Token::Type::t_semicolon)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(nametoken),
                "An extern function declaration cannot have a body - it must end with ';'");
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        cursor.skip(); // the semicolon

        funcdecl->extern_symbol = extern_symbol;

        if (!symbol_only) {
            payload.context.scope().add_funcdecl(*funcdecl);
        }

        return funcdecl;
    }

    // if we are only interested in the symbol, we are done
    if (symbol_only) {
        return funcdecl;
    }

    // we already add the function declaration to the scope
    // in case the function is recursive
    payload.context.scope().add_funcdecl(*funcdecl);

    // attach all attributes to the function
    auto attributes = payload.context.scope().collect_attributes();
    for (auto &attr : attributes) {
        funcdecl->attributes.push_back(attr);
    }

    // if next token is a semicolon we are done for now
    if (cursor.is_type(Token::Type::t_semicolon)) {
        cursor.skip();

        // a bodyless declaration gets its implementation from one of two places: an LLVM
        // intrinsic, which still becomes a real llvm::Function, or a compiler builtin, which has
        // no symbol at all and whose call sites fold to a constant
        if (auto *intrinsic_attr = funcdecl->attributes.get_first("intrinsic")) {
            auto value = attribute_string_value(payload, intrinsic_attr, "intrinsic");
            if (!value) {
                return nullptr;
            }
            funcdecl->intrinsic = value;
        }

        if (auto *builtin_attr = funcdecl->attributes.get_first("builtin")) {
            auto value = attribute_string_value(payload, builtin_attr, "builtin");
            if (!value) {
                return nullptr;
            }

            if (!AST::is_known_builtin(value.value())) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(builtin_attr->attribute_tokens),
                    fmt::format("Unknown compiler builtin '{}'.", value.value()));
                return nullptr;
            }

            funcdecl->builtin = value;
        }

        return funcdecl;
    }

    // if the next token is an open brace, we parse the function body
    if (!cursor.is_type(Token::Type::t_open_brace)) {
        payload.collect_unexpected_token(Token::Type::t_open_brace);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the open brace
    cursor.skip();

    // push the function scope
    payload.context.push_scope(funcscope);

    {
        // the body's returns fit this type, exactly as a variable declaration's initializer fits
        // the declared variable type. scoped so a declaration nested in the body restores it
        AST::ReturnTypeScope return_scope(payload.context, funcdecl->return_type);

        // the receiver reaches the body as an ordinary parameter, so the *body* is no longer inside
        // a struct declaration. cleared rather than left standing so nothing declared in here
        // inherits a receiver it has no business having
        AST::SelfScope no_self(payload.context, nullptr, nullptr);

        funcdecl->body = &parse_scope(payload);
    }

    // we expect a closing brace
    if (!cursor.is_type(Token::Type::t_close_brace)) {
        payload.collect_unexpected_token(Token::Type::t_close_brace);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the closing brace
    cursor.skip();

    // pop the function scope
    payload.context.pop_scope();

    // payload.context.scope().children.push_back(AST::make_ref(funcdecl));
    return funcdecl;
}