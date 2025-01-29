#include "Parser/TypeDeclParser.h"

#include "AST/ASTFunctionRegistry.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/AssignNode.h"
#include "AST/ReturnNode.h"
#include "AST/VarRefNode.h"
#include "AST/VarNode.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/VarDeclParser.h"
#include "Parser/ScopeParser.h"
#include "Parser/SymbolParser.h"
#include "Parser/TypeParser.h"

#include <algorithm>
#include <fmt/core.h>

// the node a previous pass already registered for this declaration site, with its arguments dropped,
// or null when the running pass is the first to reach it
//
// a module is parsed in several passes over identical token indices, so the declaration *site* is
// exact - and unlike the name, which every overload of a set shares, it identifies one declaration
// the arguments are rebuilt against the running pass's context rather than kept, because the body's
// reads have to bind to declarations from the same pass
static AST::FunctionDeclNode *reconciled_member_decl(Parser::Payload &payload, const TokenReference &site_token)
{
    AST::FunctionDeclNode *decl = payload.collector.functions.find_by_declaration_site(site_token);

    if (decl != nullptr) {
        decl->args.clear();
    }

    return decl;
}

// opens a member body, and answers whether there is one to parse here. false means the caller is done
// with this member: either the brace is missing and has been reported, or this is the declaration pass,
// which takes the signature and skips the body whole
//
// the one place that decides where a member body begins, shared by the constructor and destructor arms
// so a change to the recovery or to which pass reads a body cannot reach only one of them
static std::optional<TokenReference> enter_member_body(Parser::Payload &payload, AST::FunctionDeclNode *decl)
{
    if (!payload.expect_token(Token::Type::t_open_brace)) {
        return std::nullopt;
    }

    // answers with the brace rather than a bare yes, because the caller has to hand it to parse_scope:
    // it is the identity of this body block's lexical namespace, and the declaration pass below keys the
    // same block on the same token
    const TokenReference brace = payload.cursor.current();

    if (payload.pass == Parser::Pass::t_declarations) {
        // walked rather than skipped, so a declaration written inside a member body joins its overload
        // set before any call to it is parsed - the body pass is a single linear walk, and a free call
        // that cannot be resolved is reported and *discarded* right there, with nothing left for the
        // fixpoint to retry. a member body would otherwise be the one place a forward reference to a
        // block-local helper failed
        //
        // the same frames the body pass opens, for the same reason parse_funcdecl's descent opens them:
        // the two passes have to read a nested declaration identically, and the receiver reaches this
        // body as an ordinary parameter - so a `function` written in here is a free function, not
        // another member of the owner
        AST::FunctionBodyScope body_scope(payload.context, decl);

        Parser::parse_declaration_surface(payload, brace);
        return std::nullopt;
    }

    payload.cursor.skip(); // skip "{"

    return brace;
}

// closes a member body: the brace, and the scope enter_member_body's caller pushed. reported and left
// unconsumed when the brace is missing, which is what lets the enclosing struct walk recover
static void leave_member_body(Parser::Payload &payload)
{
    if (payload.expect_token(Token::Type::t_close_brace)) {
        payload.cursor.skip(); // skip "}"
    }

    payload.context.pop_scope();
}

// gives a constructor's `$this` its storage. a struct's is a plain stack slot that gen_var_decl
// zero-fills; a class's is a fresh heap block with its strong count already at 1.
//
// that one initializer is the entire difference between constructing the two storage classes -
// everything after it, the property writes and the implicit `return $this`, is shared code. shared by
// both the user-written and the synthesized constructor so the two cannot diverge
static void seat_this_storage(
    Parser::Payload &payload,
    AST::VarDeclNode *this_vardecl,
    const AST::ValueType &self_value_type,
    const TokenReference &name_token)
{
    if (!self_value_type.is_class()) {
        return;
    }

    this_vardecl->init_expr =
        payload.context.emplace_nodep<AST::ClassAllocExprNode>(self_value_type, name_token);
}

// recognises the copy constructor among a struct's constructors, and reports the two ways of getting
// it wrong. called from parse_constructor as soon as the signature is complete
//
// recognition rather than declaration: `constructor(Foo& $other)` already parses, registers and
// resolves - `Foo($a)` calls it today - so all this does is publish *which* of a type's constructors
// the copy is, for AST::OwnershipPass to reach by type when it inserts an implicit one. that is also
// why the declaration stays in the overload set, unlike a destructor's: the explicit call and the
// implicit copy have to be the same declaration or there are two ways to copy a value
//
// the synthesized field-wise constructor cannot be mistaken for one and needs no flag saying so,
// because it never comes through here. worth being deliberate about: for
// `struct Odd { Odd& $other; }` the two are signature-identical, and the field-wise one copies a
// borrow rather than duplicating anything
static void publish_copy_constructor(
    Parser::Payload &payload,
    AST::TypeDeclNode *struct_node,
    const AST::ValueType &self_value_type,
    AST::FunctionDeclNode *ctor_decl,
    const TokenReference &ctor_token)
{
    if (AST::is_copy_constructor(ctor_decl, self_value_type)) {
        AST::FunctionDeclNode *existing = struct_node->complex_type().copy_constructor();

        // asked of the *slot* and compared by identity, so the body pass arriving at the node the
        // declaration pass registered is not a redeclaration - register_function's declaration-site
        // claim is what tells those two apart. the same shape parse_destructor uses below
        if (existing == nullptr || existing == ctor_decl) {
            struct_node->complex_type().set_copy_constructor(ctor_decl);
            return;
        }

        // an identically-shaped second one has already been reported at this very token, as
        // register_function's DuplicateFunctionSignature. only a *differently* shaped one reaches
        // here - `Foo&` beside `const Foo&`, which are two signatures and both answer the
        // recognition rule - and that does need saying, because they are not overloads of anything:
        // a type has one copy, and nothing would decide which of them an implicit copy meant
        //
        // and deliberately no skip_declaration_body/return, unlike the destructor's duplicate below.
        // report_bodyless at the end of parse_typedecl iterates *every* constructor, so a body-less
        // one would earn a second, confusing diagnostic; the destructor escapes that because
        // report_bodyless asks only its slot
        if (!AST::signatures_match(existing, ctor_decl->parameter_types())) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(ctor_token),
                fmt::format("'{}' already has a copy constructor.", struct_node->type_name()));
        }

        return;
    }

    // `Box& $o` inside `struct Box<T>` is *not* the copy constructor and is almost certainly meant to
    // be one. an unqualified name resolves to the template, which is not a type any value has, so the
    // parameter cannot be the borrow of `Box<T>` and the recognition above rightly declines it
    // reported here because the alternative is silence: nothing else would fire until an implicit copy
    // is rejected somewhere else entirely, naming a type the author believes they wrote a copy for
    if (!self_value_type.has_complex_type() || ctor_decl->args.size() != 1) {
        return;
    }

    const AST::ComplexType *self_ct = self_value_type.get_complex_type();
    const AST::ValueType param = ctor_decl->parameter_type(0);

    if (self_ct->template_ref == nullptr || !param.is_pointer() || param.is_nullable()) {
        return;
    }

    if (!param.pointee().has_complex_type() || param.pointee().get_complex_type() != self_ct->template_ref) {
        return;
    }

    payload.collector.collect_issue<AST::Issue::GenericError>(
        payload.context.code_ref(ctor_decl->args[0]->token_varname),
        fmt::format(
            "'{}' names the template rather than a type, so this is not a copy constructor for '{}'. "
            "Write '{}&' to declare one.",
            struct_node->type_name(),
            self_value_type.get_type_desciption(),
            self_value_type.get_type_desciption()));
}

// a `constructor(...)` written in a struct body. registered in the declaration pass and given its
// body in the body pass, exactly as parse_funcdecl handles a function: the two passes reconcile on
// the declaration site, and the arguments are rebuilt against whichever pass is running so the
// body's reads bind to declarations from the same pass
static void parse_constructor(
    Parser::Payload &payload,
    AST::TypeDeclNode *struct_node,
    const AST::ValueType &self_value_type)
{
    auto &cursor = payload.cursor;
    auto name_token = struct_node->name_token.value();

    // this declaration's site: its own `constructor` keyword, not the struct's name token, which
    // every constructor of the struct shares. see FunctionDeclNode::declaration_site_token()
    auto ctor_token = cursor.current();

    cursor.skip(); // skip "constructor"

    if (!payload.expect_token(Token::Type::t_open_paren)) {
        return;
    }

    cursor.skip(); // skip "("

    AST::FunctionDeclNode *ctor_decl = reconciled_member_decl(payload, ctor_token);

    if (ctor_decl == nullptr) {
        ctor_decl = &payload.context.emplace_node<AST::FunctionDeclNode>(name_token, ctor_token);
        ctor_decl->member_kind = AST::MemberKind::t_constructor;
        struct_node->add_constructor(ctor_decl);
    }

    // the declaring namespace, like the struct's own: `Foo(...)` has to resolve wherever the type name
    // does, and a type is not block-scoped
    ctor_decl->ast_namespace = payload.context.declaring_namespace();

    // share the struct's parameter declarations rather than declaring its own: the ctor's return
    // type is the struct's self-application Foo<T>, so a substitution built from this list has to
    // bind the very same T that type mentions
    ctor_decl->type_parameters = struct_node->type_parameters();

    // the return type is the one thing *not* rebuilt per pass: it is the struct's interned self type,
    // so the second pass would only emplace a node equal to the one already here
    if (ctor_decl->return_type == nullptr) {
        ctor_decl->return_type = &payload.context.emplace_node<AST::TypeNode>(self_value_type);
    }

    // ctor argument + body scope
    auto &ctor_scope = payload.context.emplace_node<AST::ScopeNode>();

    if (!parse_parameter_list(payload, *ctor_decl, ctor_scope, name_token)) {
        return;
    }

    // the signature is complete, so this is the earliest point the declaration can join its overload
    // set. registering in both passes is intentional and cheap: the declaration pass makes the
    // constructor visible to a `Foo(...)` written above it or in another file - which is the whole
    // reason it happens here rather than in the body pass - and the body pass finds its own
    // declaration site already claimed and carries on with the same node
    //
    // reported at the `constructor` keyword rather than at the struct's name, so two constructors of
    // one struct do not both point at the same place
    payload.collector.functions.register_function(
        payload.collector, payload.context.code_ref(ctor_token), ctor_decl);

    // after the registration, not instead of it: a copy constructor is a name a call site may spell,
    // so it joins the overload set exactly as any other constructor does and the slot only records
    // which one it is. both passes reach this and both compute the same answer, since the parameter
    // list above was rebuilt identically
    publish_copy_constructor(payload, struct_node, self_value_type, ctor_decl, ctor_token);

    const auto ctor_brace = enter_member_body(payload, ctor_decl);
    if (!ctor_brace.has_value()) {
        return;
    }

    // predeclare "$this" so member access works in the body. a body-local of *value* type, unlike a
    // method's `$this`, which is a borrow parameter - a constructor hands back a new instance
    auto this_token = payload.context.make_virtual_token("$this", Token::Type::t_varname, name_token);
    auto this_vardecl = payload.context.emplace_nodep<AST::VarDeclNode>(this_token, ctor_decl->return_type);
    seat_this_storage(payload, this_vardecl, self_value_type, name_token);
    auto this_var = payload.context.emplace_nodep<AST::VarNode>(this_vardecl);
    auto this_ref = payload.context.emplace_nodep<AST::VarRefNode>(this_var);

    // the body opens with an implicit `Foo $this;`, exactly as the synthesized field-wise
    // constructor below does. seeding it here rather than appending it after the body is parsed is
    // what makes it the first child, and the first child is the only position that works: gen_scope
    // allocas in child order, so a `$this` declared last has no alloca yet when the statements above
    // it read it, and CloneContext::rebind resolves to the *original* for anything not yet cloned,
    // so an instantiated generic ctor would bind its `$this` reads to the template's declaration
    auto &ctor_body = payload.context.emplace_node<AST::ScopeNode>();
    ctor_body.add_vardecl(*this_vardecl);

    // the parameters' frame is where this constructor ends: a name resolved past it belongs to
    // another function's storage - see ScopeNode::lookup_variable
    ctor_scope.is_function_boundary = true;

    // pushed under the body, so a parameter read in the body resolves through the scope's parent
    payload.context.push_scope(ctor_scope);

    {
        // same as a function body: a `return` inside the ctor fits the ctor's return type
        AST::ReturnTypeScope return_scope(payload.context, ctor_decl->return_type);

        // `$this` is a body-local here, so the body is no longer inside a struct declaration - a
        // `function` written in it is a scoped free function, exactly as in a method body
        AST::FunctionBodyScope body_scope(payload.context, ctor_decl);

        // and `$this` is fresh storage for as long as this body lasts, so a write to one of its
        // fields is that field's first write - see Context::ctor_this_ptr. *after* the frame above,
        // which clears it: this body has one, the declarations nested in it do not
        AST::ConstructorScope ctor_this_scope(payload.context, this_vardecl);

        ctor_decl->body = &parse_scope(payload, &ctor_body, ctor_brace);
    }

    leave_member_body(payload);

    // append implicit "return $this" if the user did not return
    const bool has_return = std::any_of(
        ctor_decl->body->children.begin(),
        ctor_decl->body->children.end(),
        [](const AST::NodeReference &child) { return child.has_type<AST::ReturnNode>(); });

    if (!has_return) {
        auto ret_stmt = payload.context.emplace_nodep<AST::ReturnNode>(this_ref);
        ctor_decl->body->children.push_back(AST::make_ref(ret_stmt));
    }

    payload.context.declaration_scope().add_funcdecl(*ctor_decl);
}

// a `destructor()` written in a struct body. shaped like a *method*, not like a constructor: the
// receiver is the borrow `Foo&` so the body mutates the caller's storage, the return type is void,
// and it registers on the type rather than in a namespace overload set - which is also what keeps a
// `destructor` out of every diagnostic about a name nobody declared
//
// the two-pass reconciliation is the constructor's, keyed on the `destructor` keyword token. that
// token is both the declaration site *and* the name token, so `func_name()` is "destructor" and the
// mangled name reads `_MBuffer_destructorZ...` - unambiguous because `destructor` is a keyword and
// no user member can be spelled it
//
// nobody ever writes a call to one. it is reached only by the drop calls AST::OwnershipPass inserts,
// which look it up through AST::find_destructor
static void parse_destructor(
    Parser::Payload &payload,
    AST::TypeDeclNode *struct_node,
    AST::TypeNode *self_type_node)
{
    auto &cursor = payload.cursor;
    auto dtor_token = cursor.current();

    cursor.skip(); // skip "destructor"

    if (!payload.expect_token(Token::Type::t_open_paren)) {
        return;
    }

    cursor.skip(); // skip "("

    AST::FunctionDeclNode *dtor_decl = reconciled_member_decl(payload, dtor_token);

    if (dtor_decl == nullptr) {
        dtor_decl = &payload.context.emplace_node<AST::FunctionDeclNode>(dtor_token);
        dtor_decl->member_kind = AST::MemberKind::t_destructor;
    }

    dtor_decl->ast_namespace = payload.context.declaring_namespace();
    dtor_decl->owner_type = &struct_node->complex_type();

    // shares the struct's parameter declarations rather than declaring its own, exactly as a method
    // does: the receiver's type is the struct's self-application `Foo<T>&`, so a substitution built
    // from this list has to bind the very same T that type mentions. every one of them is inherited -
    // a destructor has no type parameters of its own to spell, and no call site to spell them at
    dtor_decl->type_parameters = struct_node->type_parameters();
    dtor_decl->inherited_type_param_count = dtor_decl->type_parameters.size();

    // returns nothing, and there is no `: type` to read - built once, since a second pass would only
    // emplace a node equal to the one already here
    if (dtor_decl->return_type == nullptr) {
        dtor_decl->return_type = &payload.context.emplace_node<AST::TypeNode>(AST::ValueType::make_void());
    }

    // receiver + body scope
    auto &dtor_scope = payload.context.emplace_node<AST::ScopeNode>();

    Parser::push_receiver_param(payload, *dtor_decl, dtor_scope, self_type_node, dtor_token);

    // parsed rather than required-empty so a parameter list gets a located error naming what is
    // wrong, instead of "unexpected token" at whatever the first parameter happens to start with
    // the receiver is already in `args`, so anything beyond it is the user's
    if (!parse_parameter_list(payload, *dtor_decl, dtor_scope, dtor_token)) {
        return;
    }

    if (dtor_decl->args.size() > dtor_decl->implicit_arg_count()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(dtor_token),
            fmt::format("A destructor takes no parameters - '{}' declares {}.",
                struct_node->type_name(),
                dtor_decl->args.size() - dtor_decl->implicit_arg_count()));

        // the extra parameters are dropped rather than carried: nothing ever passes an argument to a
        // destructor, so a signature that declares one would fail at the drop site instead of here
        dtor_decl->args.resize(dtor_decl->implicit_arg_count());
    }

    // a declared return type is rejected outright rather than checked against void. `destructor() :
    // void` reads as though the colon were meaningful, and it never is
    if (cursor.is_type(Token::Type::t_colon)) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(dtor_token),
            "A destructor returns nothing - drop the ': type'.");
        cursor.skip(); // the colon
        if (can_parse_type(payload)) {
            parse_type(payload);
        }
    }

    // a struct has at most one. asked before registering, and asked of the *slot* rather than of the
    // pass, so the body pass coming back around to the declaration pass's node is not a redeclaration -
    // register_destructor's declaration-site claim is what tells those two apart. reported at the
    // second `destructor` keyword, so the two do not both point at the same place
    AST::FunctionDeclNode *existing = struct_node->complex_type().destructor();

    if (existing != nullptr && existing != dtor_decl) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(dtor_token),
            fmt::format("'{}' already has a destructor.", struct_node->type_name()));

        Parser::skip_declaration_body(payload);
        return;
    }

    // registering in both passes is intentional: the declaration pass makes the destructor reachable
    // by a drop site anywhere in the program, and the body pass finds its own site already claimed
    payload.collector.functions.register_destructor(
        payload.collector, payload.context.code_ref(dtor_token), dtor_decl, struct_node->complex_type());

    const auto dtor_brace = enter_member_body(payload, dtor_decl);
    if (!dtor_brace.has_value()) {
        return;
    }

    // the receiver's frame is where this destructor ends - see ScopeNode::lookup_variable
    dtor_scope.is_function_boundary = true;

    // pushed so the receiver resolves through the scope's parent, as in a method body
    payload.context.push_scope(dtor_scope);

    {
        AST::ReturnTypeScope return_scope(payload.context, dtor_decl->return_type);

        // the receiver reaches the body as an ordinary parameter, so the body is no longer inside a
        // struct declaration - same reason parse_funcdecl clears it
        AST::FunctionBodyScope body_scope(payload.context, dtor_decl);

        dtor_decl->body = &parse_scope(payload, nullptr, dtor_brace);
    }

    leave_member_body(payload);

    // codegen emits a body only for the declarations in the file root's children
    payload.context.declaration_scope().add_funcdecl(*dtor_decl);
}

// the field-wise constructor, synthesized for every struct: it takes one parameter per property and
// initializes them in order. built whole, so it is the same in either pass and belongs to whichever
// one reaches the struct first
static void synthesize_field_wise_constructor(
    Parser::Payload &payload,
    AST::TypeDeclNode *struct_node,
    const AST::ValueType &self_value_type)
{
    auto name_token = struct_node->name_token.value();

    std::vector<AST::ValueType> default_ctor_params;
    default_ctor_params.reserve(struct_node->properties().size());
    for (const auto &prop : struct_node->properties()) {
        default_ctor_params.push_back(prop->type_node()->type);
    }

    // it is *not* suppressed merely because the user wrote a constructor of their own. Echo has no
    // other syntax for building a struct, so taking it away the moment a convenience constructor
    // appears would silently break every `Foo(...)` elsewhere in the program. It is suppressed only
    // when one of the user's own already occupies this exact signature
    //
    // asked of *this struct's* constructors rather than of the namespace's overload set for the
    // name. the set is the wrong question twice over: it is being filled as the module is parsed, so
    // what it holds depends on how far along we are, and it also holds every free function that
    // happens to share the struct's name, which has nothing to say about how a struct is built
    for (auto *constructor : struct_node->constructors()) {
        if (AST::signatures_match(constructor, default_ctor_params)) {
            return;
        }
    }

    // the struct's own name token is this declaration's site: nothing else claims it (a struct
    // declaration is not in the function registry) and it is the same index in every pass, so the
    // synthesized constructor needs no token minted for it either
    auto &default_ctor = payload.context.emplace_node<AST::FunctionDeclNode>(name_token);
    default_ctor.member_kind = AST::MemberKind::t_constructor;
    struct_node->set_field_wise_constructor(&default_ctor);

    default_ctor.ast_namespace = payload.context.declaring_namespace();

    // shares the struct's parameter declarations, same reason as the explicit constructor above
    default_ctor.type_parameters = struct_node->type_parameters();

    // create a type node for the return type
    auto &type_node = payload.context.emplace_node<AST::TypeNode>(self_value_type);
    default_ctor.return_type = &type_node;

    // add parameters for each struct property
    for (const auto &prop : struct_node->properties()) {
        // create a parameter with the same type as the property
        auto param_token = payload.context.make_virtual_token(prop->name(), Token::Type::t_varname, name_token);
        auto param_type = payload.context.emplace_nodep<AST::TypeNode>(prop->type_node()->type);
        auto param_var = payload.context.emplace_nodep<AST::VarDeclNode>(param_token, param_type);
        default_ctor.args.push_back(param_var);
    }

    // the function body is a scope that initializes the properties of the struct from the
    // matching parameters
    auto &ctor_body = payload.context.emplace_node<AST::ScopeNode>();
    default_ctor.body = &ctor_body;

    // allocate "$this"
    auto this_token = payload.context.make_virtual_token("$this", Token::Type::t_varname, name_token);
    auto this_vardecl = payload.context.emplace_nodep<AST::VarDeclNode>(this_token, &type_node);
    seat_this_storage(payload, this_vardecl, self_value_type, name_token);
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

        // and the parameter is *handed over*, not copied. the author wrote none of this, so there is
        // nowhere to put a `mv` - and nothing ambiguous to say: the parameter was given to this
        // constructor to be built into the struct it hands back. the only place in the language that
        // sets this, which is why a hand-written constructor still has to spell its own transfers
        member_mut->hands_over_value = true;

        default_ctor.body->children.push_back(AST::make_ref(member_mut));
    }

    // return the initialized struct instance
    auto return_stmt = payload.context.emplace_nodep<AST::ReturnNode>(this_ref);
    default_ctor.body->children.push_back(AST::make_ref(return_stmt));

    payload.collector.functions.register_function(
        payload.collector, payload.context.code_ref(name_token), &default_ctor);
}

AST::TypeDeclNode *Parser::parse_typedecl(Payload &payload)
{
    auto &cursor = payload.cursor;
    const bool declarations_only = payload.pass == Pass::t_declarations;

    if (!starts_typedecl(cursor)) {
        payload.collect_unexpected_token(Token::Type::t_struct);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    const AST::ComplexTypeKind kind = typedecl_kind(cursor);

    // skip the struct or class keyword
    cursor.skip();

    if (!payload.expect_token(Token::Type::t_identifier)) {
        return nullptr;
    }

    // fetch the struct name and skip it
    auto name_token = cursor.current();
    cursor.skip();

    // optional generic type parameters: struct Foo<T, U> { ... }
    std::vector<ParsedTypeParam> parsed_type_params = parse_type_param_list(payload);

    // next token needs to be an open brace
    if (!payload.expect_token(Token::Type::t_open_brace)) {
        return nullptr;
    }

    cursor.skip(); // skip the open brace

    // try to find the predeclared struct symbol
    auto structsymbol = payload.collector.namespaces.find_symbol(name_token.value(), *payload.context.declaring_namespace());
    AST::TypeDeclNode *struct_node = nullptr;

    // we found a name matching symbol
    if (structsymbol) {
        auto symboldecl = structsymbol->node.get_ptr<AST::TypeDeclNode>();
        if (symboldecl) {
            struct_node = symboldecl;
        }
    }

    // a name-matching symbol declared at some *other* token is a second `struct Foo` in one
    // namespace, not this pass reconciling with the declaration the symbol was made from - that case
    // matches the site. everything below here mutates the node the other declaration owns: its type
    // parameters, add_typedecl, the member walk, and a field-wise constructor that add_funcdecl
    // would push into the file root a second time (codegen then emits two bodies onto one
    // llvm::Function). so this has to return before any of it. the body is skipped rather than
    // parsed-and-discarded the way a second pass over the same struct discards it, because parsing it
    // would register its methods and constructors onto the first struct under their own
    // declaration-site keys
    if (struct_node != nullptr && !struct_node->is_declared_at(name_token)) {
        payload.collector.collect_issue<AST::Issue::TypeRedeclaration>(
            payload.context.code_ref(name_token),
            struct_node->type_name(),
            struct_node->declaration_site_token());

        // the open brace is already consumed, and this is brace-depth aware and eats the closing one,
        // so the cursor lands exactly where the normal close-brace exit leaves it
        cursor.skip_till_end_of_scope();
        return nullptr;
    }

    // still no struct we create it
    if (!struct_node) {
        struct_node = &payload.context.emplace_node<AST::TypeDeclNode>(name_token, kind);
    }

    // create the struct node
    struct_node->set_namespace(payload.context.declaring_namespace());

    // declare the generic type parameters (idempotent across the parse passes) and, when the struct
    // is generic, make them resolvable while parsing its property types
    declare_type_parameters(payload, struct_node->complex_type(), parsed_type_params);
    const std::vector<AST::TypeParamDecl *> &type_parameters = struct_node->type_parameters();
    AST::TypeParamScope type_param_scope(payload.context, type_parameters);

    // the struct as seen from inside its own body: the plain struct for a non-generic one, or the
    // self-application Foo<T...> for a generic one. giving a constructor generic type parameters +
    // this return type lets the monomorphizer instantiate it alongside Foo<int>, and a method's
    // receiver is the borrow of the same thing - so the substitution a call site builds binds the
    // very T that both mention
    AST::ValueType self_value_type = struct_node->value_type();
    if (struct_node->is_generic()) {
        auto *template_ct = self_value_type.get_complex_type();
        std::vector<AST::ValueType> self_args;
        for (const auto *param : template_ct->type_parameters) {
            self_args.push_back(AST::ValueType::make_type_param(param));
        }
        // make_complex, not make_struct: the instance carries the template's storage class, so a
        // generic *class* self-applies to a class type rather than tripping make_struct's assert
        self_value_type = AST::ValueType::make_complex(
            payload.collector.type_registry.get_or_create_instantiation(template_ct, self_args));
    }

    // the receiver type every method of this struct shares: the non-nullable borrow `Foo&`. a
    // borrow rather than a value so a method reads and writes the caller's storage, and so the
    // receiver reaches the callee as an address the way any other borrow parameter does
    auto &self_type_node = payload.context.emplace_node<AST::TypeNode>(
        AST::ValueType::make_pointer(self_value_type, false));

    // makes a `function` in this body parse as a method: parse_funcdecl reads the struct off the
    // context to bind `$this`, prefix the owner's type parameters and register on the type. it opens
    // a null frame of its own around each body, so nothing nested inherits the receiver. both passes
    // reach a method, so both need it
    AST::SelfScope self_scope(payload.context, struct_node, &self_type_node);

    // add the struct to the current scope, which is the list --print-ast walks. the body pass only:
    // the declaration pass has no file root, and a name is already resolvable without this because
    // parse_type_names pushed it as a namespace symbol - which is also what lets a struct be
    // recursive, and what lets a property name a struct declared further down
    if (!declarations_only) {
        payload.context.scope().add_typedecl(*struct_node);
    }

    // create an empty base scope for the properties to sit in
    auto &structscope = payload.context.emplace_node<AST::ScopeNode>();

    // only the first pass to walk this body keeps what it parsed - see TypeDeclNode's
    // members_collected(). the second walks the same code, rather than skipping to each member's end,
    // for two reasons: the two cannot then disagree about where a member ends, and re-reading a
    // property type calls TypeRegistry::get_or_create_instantiation again, which is what *refreshes*
    // a generic application interned during the declaration pass before its template had properties
    // (the stale-instance path in get_or_create_instantiation). the re-read's own VarDeclNode is
    // discarded; the refresh is a side effect on the registry, which is global
    const bool collect_members = !struct_node->members_collected();

    while (!cursor.is_done()) {
        if (starts_vardecl(payload)) {
            auto var = parse_varexpr(payload, &structscope);

            // append the var as a property of the struct
            if (var && collect_members) {
                struct_node->add_property(var);
            }
        }
        else if (starts_funcdecl(cursor)) {
            // a method. the declaration lands in the *enclosing* scope's children rather than in the
            // struct - the struct body never pushes a scope, so `payload.context.scope()` is still
            // the file/namespace root, which is the list codegen walks to emit bodies. exactly how a
            // constructor already reaches codegen; being a member is a lookup rule, not a placement
            // the declaration pass's body is parse_funcdecl's own to consume: it walks the body's
            // declaration surface so a `function` nested in a method joins its block's overload set.
            // skipping it here as well would eat the token after the body
            parse_funcdecl(payload);
        }
        else if (cursor.is_type(Token::Type::t_identifier) && cursor.current().value() == "constructor") {
            parse_constructor(payload, struct_node, self_value_type);
        }
        else if (cursor.is_type(Token::Type::t_destructor)) {
            // a real token, unlike `constructor` above: it has to be one so no member can be named
            // `destructor` and collide with the mangled name of the real thing
            parse_destructor(payload, struct_node, &self_type_node);
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

    struct_node->mark_members_collected();

    // synthesized by whichever pass walked the body, for the same reason the properties belong to it:
    // there is one field-wise constructor per struct, and its parameter list is the layout this walk
    // just collected. after the walk, so the suppression rule sees every constructor the user wrote -
    // however far down the body they wrote it
    if (collect_members) {
        synthesize_field_wise_constructor(payload, struct_node, self_value_type);
    }

    if (!declarations_only) {
        // a member the declaration pass registered that this pass never reached - error recovery in
        // the body above skipped past it. it would otherwise be silent and fatal: codegen *declares*
        // every function in the module's arena but emits a body only for the ones in the file root's
        // children, so the program would link against a symbol nobody defines
        auto report_bodyless = [&payload](AST::FunctionDeclNode *member) {
            if (member == nullptr || member->body != nullptr) {
                return;
            }

            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(member->declaration_site_token()),
                fmt::format("'{}' was declared but never given a body.", member->signature_description()));
        };

        for (auto *ctor : struct_node->constructors()) {
            report_bodyless(ctor);
        }

        report_bodyless(struct_node->complex_type().destructor());

        // codegen emits a body from the file root's children, so the synthesized constructor has to
        // land there - at the tail, where it has always been
        if (struct_node->field_wise_constructor() != nullptr) {
            payload.context.declaration_scope().add_funcdecl(*struct_node->field_wise_constructor());
        }
    }

    return struct_node;
}
