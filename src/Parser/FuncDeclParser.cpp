#include "Parser/FuncDeclParser.h"

#include "AST/VarDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/ASTBuiltin.h"
#include "AST/ExprNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/VarNode.h"
#include "AST/VarRefNode.h"

#include "Parser/TypeParser.h"
#include "Parser/ExprParser.h"
#include "Parser/VarDeclParser.h"
#include "Parser/ScopeParser.h"
#include "Parser/SymbolParser.h"

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

void Parser::push_implicit_param(
    Parser::Payload &payload,
    AST::FunctionDeclNode &decl,
    AST::ScopeNode &into,
    const std::string &name,
    AST::TypeNode *type_node,
    const TokenReference &at)
{
    auto token = payload.context.make_virtual_token(name, Token::Type::t_varname, at);
    auto *vardecl = payload.context.emplace_nodep<AST::VarDeclNode>(token, type_node);

    // ahead of everything the caller wrote. FunctionDeclNode::clone clones args before the body
    // precisely so the body's references rebind to the cloned declaration
    decl.args.push_back(vardecl);

    // declared in the argument scope, not in the body, so it resolves the same way every other
    // parameter does. the argument scope's children are never emitted - the body is a separate child
    // scope and codegen allocas from `args` - so this costs no second slot
    into.add_vardecl(*vardecl);
}

bool Parser::parse_function_body(
    Parser::Payload &payload,
    AST::FunctionDeclNode &decl,
    AST::ScopeNode &scope,
    AST::ClosureExprNode *closure)
{
    auto &cursor = payload.cursor;

    if (!payload.expect_token(Token::Type::t_open_brace)) {
        return false;
    }

    // the body's opening brace, captured before it is consumed: it is the identity of the body block's
    // lexical namespace, and the declaration pass keyed the same block on the same token
    auto body_brace = cursor.current();

    cursor.skip(); // the opening brace

    // the parameters live one frame up from the body, and that frame is where this function ends. a
    // name resolved past it belongs to another function's storage - see ScopeNode::lookup_variable.
    // for a closure that is exactly what makes such a name a *capture* instead
    scope.is_function_boundary = true;

    payload.context.push_scope(scope);

    {
        // the body's returns fit this type, exactly as a variable declaration's initializer fits
        // the declared variable type. scoped so a declaration nested in the body restores it
        AST::ReturnTypeScope return_scope(payload.context, decl.return_type);

        // the receiver reaches the body as an ordinary parameter, so the *body* is no longer inside a
        // struct declaration - and nothing enclosing this declaration reaches into it either. cleared
        // rather than left standing so nothing declared in here inherits a receiver, an environment or a
        // constructor's `$this` it has no business having, and it names the lexical namespaces the body
        // opens so a diagnostic about a block-local declaration can say which function it was written in
        //
        // the closure is the one thing passed *on* rather than cleared, and only to its own body
        AST::FunctionBodyScope body_scope(payload.context, &decl, closure);

        decl.body = &parse_scope(payload, nullptr, body_brace);
    }

    if (!payload.expect_token(Token::Type::t_close_brace)) {
        return false;
    }

    cursor.skip(); // the closing brace

    payload.context.pop_scope();

    return true;
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

bool Parser::starts_funcdecl(Parser::Cursor &cursor)
{
    return cursor.is_type_sequence(0, { Token::Type::t_function, Token::Type::t_identifier });
}

bool Parser::starts_closure_literal(Parser::Cursor &cursor)
{
    return cursor.is_type_sequence(0, { Token::Type::t_function, Token::Type::t_open_paren });
}

void Parser::skip_declaration_body(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    if (cursor.is_type(Token::Type::t_open_brace)) {
        cursor.skip();
        cursor.skip_till_end_of_scope();
        return;
    }

    if (cursor.is_type(Token::Type::t_semicolon)) {
        cursor.skip();
    }
}

// recovery for a `function` this parser has decided not to read: past its signature and its whole body
static void skip_refused_function(Parser::Payload &payload)
{
    // the signature cannot contain either token, so the first one found opens the body or ends a
    // bodyless declaration - and from there skip_declaration_body knows how to consume it
    payload.cursor.skip_until({ Token::Type::t_open_brace, Token::Type::t_semicolon });

    Parser::skip_declaration_body(payload);
}

// the environment parameter. `args[0]` of every closure, exactly where a method's receiver sits and for
// the same reason: it is how the body reaches storage the caller does not pass, so it must be a real
// parameter rather than something codegen conjures
//
// typed as the environment *class*, so a captured read is an ordinary member access and the whole
// property-offset machinery is reused rather than rebuilt. a class value is one machine word, so this
// lowers to the same `ptr` the callable's env slot holds. the *type of the closure* says nothing about
// it - two closures of one signature capturing different things are one type
static void push_environment_param(
    Parser::Payload &payload,
    AST::FunctionDeclNode &decl,
    AST::ScopeNode &into,
    AST::ComplexType *environment,
    const TokenReference &at)
{
    auto &env_type = payload.context.emplace_node<AST::TypeNode>(AST::ValueType::make_class(environment));

    Parser::push_implicit_param(payload, decl, into, "$__env", &env_type, at);
}

AST::ExprNode *Parser::capture_variable(
    Parser::Payload &payload,
    AST::VarDeclNode *vardecl,
    const TokenReference &at,
    size_t boundaries_crossed)
{
    AST::ClosureExprNode *closure = payload.context.current_closure_ptr;

    assert(closure != nullptr && closure->decl != nullptr && "capture_variable called outside a closure body");

    // the environment is `args[0]`, put there by push_environment_param before the parameter list was
    // read - the same slot a method's receiver sits in, which is what implicit_arg_count counts
    AST::VarDeclNode *env_param = closure->decl->args[0];

    // more than one frame out means the closure this is written in would have to capture it *and* hand it
    // on. the value has to be read where it lives, and that place is not reachable from the creation site
    // of this closure - so it is refused rather than read from the wrong frame
    if (boundaries_crossed > 1) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(at),
            fmt::format(
                "'{}' is declared outside the closure that encloses this one. Capturing through a "
                "closure is not supported yet - capture it in the outer closure first.",
                at.value()));
        return nullptr;
    }

    // whether the captured value owns a resource is *not* asked here, though this is where the capture
    // is made: a variable's type is only final once the monomorphizer has settled the call it was
    // inferred from, so the answer for `$b = Box<int32>(5)` would be taken from `Box<T>` and come back
    // "owns nothing". AST::TypeChecker::visit_closure_expr asks it, once the types are honest
    //
    // a declaration whose inference failed has no type node at all, and `type()` would read through the
    // null. unknown rather than a refusal: the failure has already been reported at the declaration, and
    // "no information" is what every pass below reads an unresolved type as - the same answer
    // VarRefNode::result_type gives for the same declaration
    const AST::ValueType captured_type =
        vardecl->has_type() ? vardecl->type() : AST::ValueType::make_unknown();

    const AST::ValueType env_type = env_param->type();
    AST::ComplexType *environment = env_type.get_complex_type();

    const std::string property_name = vardecl->token_varname.value();

    // already captured, so the property is reused rather than added twice. two reads of one variable are
    // one capture - which is also what keeps the property indices in step with `captured_values`
    if (!environment->has_property(property_name)) {
        environment->add_property(property_name, captured_type);

        // the place, read in the *enclosing* frame. it is an ordinary VarRef over the outer declaration,
        // and it is evaluated at the closure expression rather than in the body - which is the whole of
        // what "by value" means here
        auto &outer_var = payload.context.emplace_node<AST::VarNode>(vardecl, at);
        auto &outer_ref = payload.context.emplace_node<AST::VarRefNode>(&outer_var);

        closure->captured_values.push_back(&outer_ref);
    }

    // and the read the body gets: `$__env->name`. the environment is a class, so its value is already a
    // handle - the same shape a member access on any class value has
    auto &env_var = payload.context.emplace_node<AST::VarNode>(env_param, at);
    auto &env_ref = payload.context.emplace_node<AST::VarRefNode>(&env_var);

    return &payload.context.emplace_node<AST::MemberAccessNode>(AST::make_ref(env_ref), at);
}

AST::ClosureExprNode *Parser::parse_closure_literal(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    assert(starts_closure_literal(cursor) && "parse_closure_literal called off a closure literal");

    // the `function` keyword is both the declaration site and, through a virtual name token, where the
    // symbol's position is reported from. a closure has no name the user wrote, so the two coincide the
    // way a destructor's do
    auto function_token = cursor.current();
    cursor.skip();

    // a name nobody can spell, discriminated so two closures never share a symbol. the block's lexical
    // namespace already separates blocks; the counter separates two literals inside one block
    auto name_token = payload.context.make_virtual_token(
        fmt::format("closure${}", payload.collector.next_closure_id++),
        Token::Type::t_identifier,
        function_token);

    auto *closure_decl = payload.context.emplace_nodep<AST::FunctionDeclNode>(name_token, function_token);

    closure_decl->is_closure = true;

    // the block it was written in, which is what makes its symbol unique per block and lets a diagnostic
    // say which function it appeared in. deliberately *not* registered in the FunctionRegistry: no name
    // reaches a closure, so it belongs to no overload set
    closure_decl->ast_namespace = payload.context.current_namespace;

    // the same reason a nested declaration is rejected where a type parameter is visible: a closure
    // cannot receive a substitution for one. lifted with capture-in-generics, see todo/A27
    if (payload.context.has_visible_type_params()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(function_token),
            "A closure cannot be written inside a generic function's body yet - it has no access to the "
            "enclosing type parameters.");
        skip_refused_function(payload);
        return nullptr;
    }

    auto &closure_scope = payload.context.emplace_node<AST::ScopeNode>();

    // the environment is created empty and grows a property per capture as the body is parsed. a class,
    // so it is heap allocated and reference counted - two copies of one closure share one environment,
    // which is what makes assigning and returning a closure mean what it should
    //
    // minted even when nothing turns out to be captured: the body is what says, and the parameter has to
    // be typed before the body is read. an environment with no properties is simply never allocated
    AST::ComplexType *environment = payload.collector.type_registry.create_anonymous_type(
        closure_decl->func_name() + ".env",
        AST::ComplexTypeKind::t_class,
        payload.context.declaring_namespace());

    // `args[0]`, ahead of everything the user wrote - which is where capture_variable reads it back from
    push_environment_param(payload, *closure_decl, closure_scope, environment, function_token);

    cursor.skip(); // the open paren

    if (!parse_parameter_list(payload, *closure_decl, closure_scope, function_token)) {
        return nullptr;
    }

    // `: T` is optional here exactly as it is on a declaration, where a missing return type is void
    if (cursor.is_type(Token::Type::t_colon)) {
        cursor.skip();

        if (!can_parse_type(payload)) {
            payload.collect_unexpected_token(Token::Type::t_identifier);
            cursor.try_skip_to_next_statement();
            return nullptr;
        }

        closure_decl->return_type = parse_type(payload);
    }

    // the node exists before the body is parsed, because the body is what fills its capture list. it is
    // also what the body is parsed *under*, which is what makes a read of an enclosing local in there a
    // capture rather than the error it is inside a plain nested `function`
    auto &closure_expr = payload.context.emplace_node<AST::ClosureExprNode>(closure_decl, function_token);

    if (!parse_function_body(payload, *closure_decl, closure_scope, &closure_expr)) {
        return nullptr;
    }

    // to the file root, like every other declaration: codegen emits bodies from there and
    // AST::OwnershipPass resolves drops from the same list
    payload.context.declaration_scope().add_funcdecl(*closure_decl);

    // only now is it known whether anything was captured. an environment with no properties is left off
    // the node entirely, so the closure's env slot stays null and nothing is allocated for it
    if (environment->property_count() > 0) {
        closure_expr.environment_type = environment;
    }

    return &closure_expr;
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
    // and the Echo name are the same
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

    // a `function` written inside a `{ }` block is a scoped declaration - the lexical namespace above
    // is the block's - and it has no access to the enclosing frame. an enclosing *type parameter* is
    // exactly such an access: `T` would resolve through the type-param scope stack and silently make
    // this declaration depend on a substitution nothing will ever hand it, so it is rejected outright
    // rather than lowered against an unresolved generic. see todo/A27, which lifts this
    if (payload.context.current_namespace != nullptr
        && payload.context.current_namespace->is_lexical()
        && payload.context.has_visible_type_params())
    {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(nametoken),
            fmt::format(
                "'{}' cannot be declared inside a generic function's body - it has no access to the "
                "enclosing type parameters. Declare it at file scope instead.",
                nametoken.value()));
        skip_refused_function(payload);
        return nullptr;
    }

    // a `function` written inside a struct body is a method: the enclosing struct arrived on the
    // context, and it is the only thing that distinguishes this from a free function
    AST::TypeDeclNode *owner_struct = payload.context.self_struct_ptr;
    AST::TypeNode *self_type = payload.context.self_type_ptr;
    const bool is_method = owner_struct != nullptr && self_type != nullptr;

    // a method of a generic struct carries the owner's parameters ahead of its own, so that one
    // TypeSubstitution binds both: the owner's T from the receiver argument, its own U from the rest
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
        // a raw symbol has exactly one definition, so it cannot have one body per instantiation
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
            payload.context.declaration_scope().add_funcdecl(*funcdecl);
        }

        return funcdecl;
    }

    // the declaration pass takes the signature and then walks the *declarations* of the body, so a
    // `function` written inside this one joins its block's overload set before any call to it is read.
    // consumed here rather than by the caller: both of this pass's callers - the file walk and the
    // struct member walk - want exactly this, and the body is this function's to know the shape of
    if (symbol_only) {
        if (cursor.is_type(Token::Type::t_open_brace)) {
            // the frames this opens have to be *exactly* the ones the body pass opens below, or the two
            // passes read one declaration differently and the first to reach it wins: without the null
            // self a `function` nested in a method body registers as another method of the owner here,
            // and the body pass then finds the site already claimed and never puts it in an overload set
            // at all - the name resolves nowhere. one guard, so the two sites cannot drift apart
            //
            // opened before the surface walk, because the block's lexical namespace is named after this
            // function and the walk mints it
            AST::FunctionBodyScope body_scope(payload.context, funcdecl);

            Parser::parse_declaration_surface(payload, cursor.current());
        }

        return funcdecl;
    }

    // the declaration goes to the file root, never into the body it was written in. a nested `function`
    // is a scoped declaration, not a closure: its *name* belongs to the block, while the declaration
    // itself is an ordinary top-level one. codegen emits bodies from the file root's children and
    // AST::OwnershipPass resolves drops from the same list, so one left in a body scope would be both
    // undefined at link time and never ownership-resolved
    //
    // added before the body is parsed so a recursive call inside it resolves
    payload.context.declaration_scope().add_funcdecl(*funcdecl);

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

    if (!parse_function_body(payload, *funcdecl, funcscope)) {
        return nullptr;
    }

    return funcdecl;
}