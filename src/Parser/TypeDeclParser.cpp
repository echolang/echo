#include "Parser/TypeDeclParser.h"
#include "Parser/ConstDeclParser.h"
#include "Parser/OperatorDeclParser.h"

#include "AST/ASTConformance.h"
#include "AST/ASTConstructor.h"
#include "AST/ASTCoreTypes.h"
#include "AST/ASTFunctionRegistry.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ExprNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/MemberAccessNode.h"
#include "AST/AssignNode.h"
#include "AST/ReturnNode.h"
#include "AST/VarRefNode.h"
#include "AST/VarNode.h"
#include "Parser/AttributeParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/VarDeclParser.h"
#include "Parser/ScopeParser.h"
#include "Parser/SymbolParser.h"
#include "Parser/TypeParser.h"

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

// `type Iter : contract::iterator<V>;` in an interface body - an associated type: one the *implementor* chooses
// and the interface only constrains.
//
// four refusals, each declining to publish, so ComplexType::associated_types() only ever holds entries
// AST::conformance_bindings can actually solve. the cursor is consumed to the `;` on every path, so the
// rest of the body is still read
static void parse_associated_type(
    Parser::Payload &payload,
    AST::TypeDeclNode &struct_node,
    bool is_interface_body,
    bool collect_members)
{
    auto &cursor = payload.cursor;

    const auto keyword = cursor.current();
    cursor.skip(); // `type`

    const auto name_token = cursor.current();
    cursor.skip();

    // consumes to the end of the declaration, whatever happened above it
    auto finish = [&]() {
        cursor.skip_until({ Token::Type::t_semicolon });
        if (cursor.is_type(Token::Type::t_semicolon)) {
            cursor.skip();
        }
    };

    // one lambda rather than the gate and the `finish()` re-typed at every arm, which is what keeps the
    // four consistent - parse_typedecl below makes the same move for the same reason.
    //
    // **the pass gate is inside it**, and it is not optional: registration reconciles on the declaration
    // site rather than re-appending, so every one of these runs three times and only the body pass has a
    // complete enough picture to be believed
    auto refuse = [&](const TokenReference &at, std::string message) {
        if (collect_members) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(at), std::move(message));
        }

        finish();
    };

    // **only an interface declares one.** a struct *binds* an associated type its conformance declared;
    // it does not introduce one, and there would be nothing to constrain the binding against
    if (!is_interface_body) {
        refuse(keyword, fmt::format(
            "only an 'interface' declares an associated type, and '{}' is not one. an implementor "
            "binds one by declaring the member that answers the requirement mentioning it.",
            struct_node.type_name()));
        return;
    }

    AST::ComplexType &owner = struct_node.complex_type();

    // **declared before the requirements.** a position rule rather than a use-before-declare detection:
    // caught afterwards it would surface as parse_type's "unknown type 'Iter'", which points at the
    // wrong line and names the wrong problem
    //
    // **asked only in the pass that can answer it** - not merely reported there, which is what makes it
    // the one arm whose condition carries the gate rather than leaving it to `refuse`: registration
    // reconciles on the declaration site rather than re-appending, so in the body pass the method list
    // is already full and the question itself would misfire on every well-formed interface
    if (collect_members && !owner.methods().empty()) {
        refuse(keyword, fmt::format(
            "'{}' has to be declared before '{}''s requirements - a requirement's signature may "
            "mention it, so it has to be a name by the time one is read.",
            name_token.value(), struct_node.type_name()));
        return;
    }

    // a collision with one of the interface's own type parameters. name resolution takes the first
    // match, so the later one would simply be unreachable - the same reason the type-parameter list
    // refuses a repeat within itself
    for (const auto *param : owner.type_parameters) {
        if (param->name == name_token.value()) {
            refuse(name_token, fmt::format(
                "'{}' is already a type parameter of '{}', so the associated type of that name could "
                "never be named.",
                name_token.value(), struct_node.type_name()));
            return;
        }
    }

    // **reused across passes, never minted twice.** equality on a type parameter is TypeParamDecl*
    // identity, so a second declaration would make pass 2's `Iter` compare unequal to pass 3's and every
    // conformance would report unmet against two identical-looking types. declare_params does the same
    // for the same reason
    AST::TypeParamDecl *decl = owner.find_associated_type(name_token.value());

    if (decl != nullptr && collect_members) {
        refuse(name_token, fmt::format("'{}' is already an associated type of '{}'.",
            name_token.value(), struct_node.type_name()));
        return;
    }

    Parser::ParsedTypeParam parsed{ name_token, {}, "" };

    if (!Parser::parse_constraint_atoms(payload, parsed)) {
        // parse_constraint_atoms has reported
        finish();
        return;
    }

    // **unconstrained is refused.** a type parameter's argument is chosen by a caller who knows the
    // concrete type; an associated type is read by a generic body that knows only the constraint, so an
    // unconstrained one promises nothing at all and constraint_admits would accept anything. this is the
    // reversible direction - loosening later is source-compatible, tightening is not
    if (parsed.constraint.empty()) {
        refuse(name_token, fmt::format(
            "the associated type '{}' needs a constraint - write 'type {} : SomeInterface;'. a body "
            "reading it knows only what the constraint promises, so an unconstrained one would promise "
            "nothing.",
            name_token.value(), name_token.value()));
        return;
    }

    if (decl == nullptr) {
        // the ordinal is passed as 0 and meant to be: ComplexType::add_associated_type is its sole
        // minter, and it continues past the type parameters *and* the associated types already there -
        // a second expression here would only be a worse guess at the same number
        decl = payload.collector.type_params.declare(name_token.value(), 0, name_token);
        owner.add_associated_type(decl);
    }

    // refreshed on every pass, exactly as declare_params refreshes a type parameter's: the type-name
    // pass walks the atoms without resolving them, so the constraint is only real from pass 2 on
    decl->constraint = std::move(parsed.constraint);
    decl->constraint_spelling = std::move(parsed.constraint_spelling);

    if (!cursor.is_type(Token::Type::t_semicolon)) {
        payload.collect_unexpected_token(Token::Type::t_semicolon);
    }

    finish();
}

// reads `: Drawable, contract::iterable<E>` and publishes what it resolves onto the type, modelled on
// publish_implicit_conversion: each refusal declines to *publish*, so ComplexType::conformances() only
// ever holds valid, deduplicated interface types and no reader has to re-filter
//
// the cursor is restored to where it was, so the caller's own walk is unaffected - the clause was
// already skipped past to find the opening brace, and this re-reads it once the type's own parameters
// are in scope
static void parse_conformance_clause(
    Parser::Payload &payload,
    AST::TypeDeclNode &struct_node,
    const Parser::Cursor::Snapshot &clause_start,
    const TokenReference &colon_token)
{
    auto &cursor = payload.cursor;
    const auto resume = cursor.snapshot();
    cursor.restore(clause_start);

    cursor.skip(); // the colon

    AST::ComplexType &owner = struct_node.complex_type();

    // an interface conforming to an interface is interface *inheritance*, and it is refused rather
    // than half-supported: a requirement set that is the union of others needs the requirements
    // flattened into the vtable in a stable order, which is a layout decision nothing else here makes
    // yet.
    //
    // asked *before* the loop, not inside it: it is a fact about the owner, so it cannot change per
    // entry - tested per entry, `interface A : B, C` reported the same sentence twice, which is what
    // the "reported once, at the clause" this is written for means. nothing is published either way,
    // so there is no second entry worth reading
    if (owner.is_interface_kind()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(colon_token),
            fmt::format(
                "'{}' is an interface, and an interface cannot conform to another one - "
                "declare the requirements it needs directly.",
                struct_node.type_name()));

        cursor.restore(resume);
        return;
    }

    while (!cursor.is_done() && !cursor.is_type(Token::Type::t_open_brace)) {
        const TokenReference entry_token = cursor.current();
        AST::TypeNode *entry = Parser::parse_type(payload);

        if (entry == nullptr) {
            // parse_type has reported; skip to the next entry rather than spinning on one bad token
            cursor.skip_until({ Token::Type::t_comma, Token::Type::t_open_brace });
            if (cursor.is_type(Token::Type::t_comma)) {
                cursor.skip();
            }
            continue;
        }

        const AST::ValueType conformance = entry->type;

        if (!conformance.is_interface()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(entry_token),
                fmt::format(
                    "'{}' is not an interface, so '{}' cannot conform to it. Only an `interface` may "
                    "appear after ':'.",
                    conformance.get_type_desciption(), struct_node.type_name()));
        }
        else {
            // the duplicate check is what makes the published list a set, which is what lets
            // AST::conforms_to be a membership test and the runtime table a plain array - and it is
            // asked *through* conforms_to, so "does this type already conform" has one answer here and
            // at every use site. spelled as a std::find it was a second one, which inheritance would
            // have made diverge the moment the list stopped being flat
            if (AST::conforms_to(&owner, conformance)) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(entry_token),
                    fmt::format(
                        "'{}' already conforms to '{}'.",
                        struct_node.type_name(), conformance.get_type_desciption()));
            }
            else {
                owner.add_conformance(conformance);
            }
        }

        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();
            continue;
        }

        break;
    }

    cursor.restore(resume);
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

// the attributes written ahead of a constructor or a destructor: drained onto the declaration they
// were written for, then handed to the one owner of every marker that has something to say about a
// member. neither used to drain, so an attribute sat on the scope's stack until whatever declaration
// came next picked it up
//
// both halves are the shared ones - Parser::drain_attributes and Parser::publish_declaration_markers,
// the same two parse_funcdecl calls around its own registration - so no rule about an attribute is
// spelled here. every attribute name is still accepted and ignored: there is no known-attribute set
// in the compiler, and inventing one at this site would quietly close a machinery that today takes
// any identifier
static void publish_member_attributes(
    Parser::Payload &payload,
    AST::FunctionDeclNode *decl,
    AST::TypeDeclNode *struct_node,
    const TokenReference &nametoken)
{
    Parser::drain_attributes(payload, decl->attributes);
    Parser::publish_declaration_markers(payload, decl, struct_node, nametoken);
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
    if (!self_value_type.has_property_layout() || ctor_decl->args.size() != 1) {
        return;
    }

    const AST::ComplexType *self_ct = self_value_type.get_complex_type();
    const AST::ValueType param = ctor_decl->parameter_type(0);

    if (self_ct->template_ref == nullptr || !param.is_pointer() || param.is_nullable()) {
        return;
    }

    if (!param.pointee().has_property_layout() || param.pointee().get_complex_type() != self_ct->template_ref) {
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

    // once the node exists, so an attribute is read against the declaration it was written for -
    // member_kind above is the whole of what publish_implicit_conversion needs to refuse it
    publish_member_attributes(payload, ctor_decl, struct_node, ctor_token);

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
    auto &this_vardecl = AST::declare_constructor_this(
        payload.context.module, *ctor_decl->return_type, name_token);

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
        AST::ConstructorScope ctor_this_scope(payload.context, &this_vardecl);

        // the body opens with an implicit `Foo $this;`, exactly as the synthesized field-wise constructor
        // below does. handed to parse_scope rather than appended once the body is parsed for one reason
        // this site owns and one it does not: a statement in the body reads `$this` by name and a name is
        // resolved as it is parsed - and it also has to precede the statements that write through it,
        // which is AST::declare_constructor_this's rule rather than this parser's
        ctor_decl->body = &parse_scope(payload, ctor_brace, { &this_vardecl });
    }

    leave_member_body(payload);

    // and the implicit `return $this` that ends it, on the same terms as the two synthesized
    // constructors - see AST::close_constructor_body, which owns when one is owed
    AST::close_constructor_body(payload.context.module, *ctor_decl, this_vardecl);

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

    // the `destructor` keyword is both the declaration site and the name token, so it is what a
    // refused attribute locates against too
    publish_member_attributes(payload, dtor_decl, struct_node, dtor_token);

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

        dtor_decl->body = &parse_scope(payload, dtor_brace);
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

    // built from the struct's properties rather than written, so no module owns its symbol and every
    // build that sees this struct produces the same definition - see AST::function_emission_kind
    default_ctor.is_implicitly_generated = true;

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

    // allocate "$this" - ahead of every statement below, which is this body's half of
    // AST::declare_constructor_this's rule: a class's field writes store *through* the handle its
    // initializer makes
    auto &this_vardecl = AST::declare_constructor_this(payload.context.module, type_node, name_token);
    default_ctor.body->add_vardecl(this_vardecl);

    // one read of `$this` per use, never one node shared between them: a node that sits in the tree
    // twice has two parents, and every pass that rewrites a child in place - AST::OperatorRewriter on a
    // member-access base, AST::PointerAdjuster on a deref - would rewrite it once per parent. it is also
    // what lets a clone answer "already cloned" with the one clone: two parents would collapse onto it
    auto make_this_read = [&]() {
        auto *var = payload.context.emplace_nodep<AST::VarNode>(&this_vardecl);
        return payload.context.emplace_nodep<AST::VarRefNode>(var);
    };

    // for each property we create an assignment statement in the constructor body
    for (size_t i = 0; i < struct_node->properties().size(); i++) {
        auto *prop = struct_node->properties()[i];

        // create $this->prop access
        auto member_token = payload.context.make_virtual_token(prop->name(), Token::Type::t_identifier, prop->token_varname);
        AST::ExprNode *target = payload.context.emplace_nodep<AST::MemberAccessNode>(AST::make_ref(*make_this_read()), member_token);

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

    // return the initialized struct instance, through the same owner the written constructor above ends
    // by. nothing here can leave early, so the guard inside it always answers "one is owed" - it is
    // called anyway, because "when does a constructor owe a return" is not a question this site holds
    AST::close_constructor_body(payload.context.module, default_ctor, this_vardecl);

    payload.collector.functions.register_function(
        payload.collector, payload.context.code_ref(name_token), &default_ctor);
}

// `#[core: "string"]` - the stdlib telling the compiler which declared type this is. bound in both the
// declaration and the body pass; re-binding the same node is how the two reconcile, and a *different*
// node is a second declaration of one core type, which is reported.
//
// its own function, with guard clauses, rather than four nested levels inside parse_typedecl: reading
// an attribute off a declaration is not parsing a type body, and the sibling readers in FuncDeclParser
// are already shaped this way
static void bind_core_type_attribute(Parser::Payload &payload, AST::TypeDeclNode *struct_node)
{
    auto *core_attr = struct_node->attributes.get_first("core");
    if (core_attr == nullptr) {
        return;
    }

    auto value = Parser::attribute_string_value(payload, core_attr, "core");
    if (!value.has_value()) {
        return;
    }

    auto kind = AST::core_type_kind_for(value.value());
    if (!kind.has_value()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(core_attr->attribute_tokens),
            fmt::format("Unknown core type '{}'.", value.value()));
        return;
    }

    if (auto *bound = payload.collector.core_types.declaration(kind.value());
        bound != nullptr && bound != struct_node) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(core_attr->attribute_tokens),
            fmt::format("The core type '{}' is already declared as '{}'.",
                value.value(), bound->namespaced_type_name()));
        return;
    }

    payload.collector.core_types.bind(kind.value(), struct_node);
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

    // the conformance clause - `struct Point : Comparable<Point>, Hashable`. read here so the cursor is
    // right in every pass, and *resolved* only in the two later ones: an interface may be declared
    // further down or in another file, and a conformance may be a generic application, which needs the
    // type grammar. the type-name pass walks these tokens through its own loop and never gets here
    //
    // the tokens are read before the type parameters are declared below, so they are re-read after the
    // TypeParamScope is open - `: contract::iterable<E>` mentions this type's own E
    const std::optional<TokenReference> conformance_token =
        cursor.is_type(Token::Type::t_colon) ? std::optional(cursor.current()) : std::nullopt;
    const auto conformance_snapshot = cursor.snapshot();

    if (conformance_token.has_value()) {
        cursor.skip_until({ Token::Type::t_open_brace });
    }

    // next token needs to be an open brace
    if (!payload.expect_token(Token::Type::t_open_brace)) {
        return nullptr;
    }

    cursor.skip(); // skip the open brace

    // the struct this declaration is written inside, if any - which is what makes it a *member* type.
    // the same field that makes a `function` in this position a method, asked one line differently, so
    // the two can never disagree about where the walk is. a `struct` in a *method* body reads null
    // here: AST::FunctionBodyScope clears the receiver, which is A30's separate question
    AST::TypeDeclNode *owner_node = payload.context.self_struct_ptr;

    // a `struct` written inside a `{ }` block where a type parameter is visible is refused, the third
    // case of the rule parse_funcdecl already applies to a nested `function` and parse_closure to a
    // closure - same predicate, same reason: `T` resolves through the type-param scope stack and would
    // make this declaration depend on a substitution nothing hands it. A type is the case where that
    // would be *silent* rather than a link error, because there is nowhere for the substituted layout to
    // come from: TypeRegistry::get_or_create_instantiation interns one ComplexType per (template, args)
    // and this declaration is no template, so the monomorphizer's clone used to mint a second layout of
    // its own - and struct equality is ComplexType* identity, so that was one type wearing two. see
    // todo/A27, which lifts all three together
    //
    // asked here, before MemberTypeScope is opened and before the node is found or created, so the
    // refusal mutates nothing on the way out. it cannot double-report with the generic *owner* refusal
    // below: a member type's namespace comes from MemberTypeScope, which is a written namespace and so
    // never lexical
    if (payload.context.current_namespace != nullptr
        && payload.context.current_namespace->is_lexical()
        && payload.context.has_visible_type_params())
    {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(name_token),
            fmt::format(
                "'{}' cannot be declared inside a generic function's body - it has no access to the "
                "enclosing type parameters. Declare it at file scope instead.",
                name_token.value()));

        // the attributes staged ahead of this declaration are still owed a drain - an undrained one
        // attaches itself to whatever declaration comes next, which is the bug the drain below records
        AST::AttributeList refused_attributes;
        Parser::drain_attributes(payload, refused_attributes);

        // the open brace is already consumed, and this is brace-depth aware and eats the closing one,
        // so the cursor lands exactly where the normal close-brace exit leaves it
        cursor.skip_till_end_of_scope();
        return nullptr;
    }

    // a nested type's *declarations* - its constructors and methods - belong to a namespace named after
    // the owner, so `A::Inner(1)` resolves and a bare `Inner(1)` does not. opened before the node is
    // namespaced below, and held for the whole body, so everything the walk registers lands there.
    // optional because the guard is only owed when there is an owner, and it is not movable
    std::optional<AST::MemberTypeScope> member_type_scope;
    if (owner_node != nullptr) {
        member_type_scope.emplace(payload.context, payload.collector.namespaces, owner_node->complex_type());
    }

    // try to find the predeclared struct node. a nested type is in no namespace, so it is reached
    // through its owner instead - parse_type_names put it there, over every file, before this pass
    AST::TypeDeclNode *struct_node = nullptr;

    if (owner_node != nullptr) {
        struct_node = owner_node->complex_type().find_member_type_decl(name_token.value());
    }
    else if (auto *structsymbol = payload.collector.namespaces.find_symbol(
                 name_token.value(), *payload.context.declaring_namespace())) {
        // we found a name matching symbol
        struct_node = structsymbol->node.get_ptr<AST::TypeDeclNode>();
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

        // parse_type_names normally got here first, over every file, which is what makes a nested type
        // nameable from anywhere in its owner. reaching this with an owner means it did not - a struct
        // nested two levels deep, say - so register it now rather than leaving a type only this pass
        // can see
        if (owner_node != nullptr) {
            owner_node->complex_type().add_member_type(name_token.value(), struct_node);
        }
    }

    // create the struct node
    struct_node->set_namespace(payload.context.declaring_namespace());

    // the attributes written ahead of the declaration, through the same drain a function uses - an
    // undrained attribute attaches itself to whatever declaration comes next, and this site is the
    // half of that bug that used to pick up a method's `#[implicit]`
    Parser::drain_attributes(payload, struct_node->attributes);

    bind_core_type_attribute(payload, struct_node);

    if (owner_node != nullptr) {
        // part of the nested type's identity - see ComplexType::owner_type. set here rather than at
        // the two registration sites so it cannot be forgotten at one of them
        struct_node->complex_type().owner_type = &owner_node->complex_type();

        // a nested type of a *generic* owner would need one layout per instantiation the moment it
        // mentions the owner's `T`, and TypeRegistry::get_or_create_instantiation has no template to
        // mint one from - the nested declaration is not itself generic. the
        // template_or_self redirect hands every instantiation the template's single nested type, which
        // is the right answer only while that cannot happen - so refuse the whole case rather than
        // ship one that miscompiles as soon as the type is actually useful
        if (owner_node->is_generic()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(name_token),
                fmt::format(
                    "'{}' is declared inside the generic type '{}', which is not supported yet - "
                    "declare it alongside its owner instead.",
                    name_token.value(), owner_node->type_name()));
        }
    }

    // declare the generic type parameters (idempotent across the parse passes) and, when the struct
    // is generic, make them resolvable while parsing its property types
    declare_type_parameters(payload, struct_node->complex_type(), parsed_type_params);
    const std::vector<AST::TypeParamDecl *> &type_parameters = struct_node->type_parameters();
    AST::TypeParamScope type_param_scope(payload.context, type_parameters);

    // only the first pass to walk this body keeps what it parsed - see TypeDeclNode's
    // members_collected(). the second walks the same code, rather than skipping to each member's end,
    // for two reasons: the two cannot then disagree about where a member ends, and re-reading a
    // property type calls TypeRegistry::get_or_create_instantiation again, which is what *refreshes*
    // a generic application interned during the declaration pass before its template had properties
    // (the stale-instance path in get_or_create_instantiation). the re-read's own VarDeclNode is
    // discarded; the refresh is a side effect on the registry, which is global
    //
    // hoisted above the body walk because the conformance clause is kept under the very same rule: it
    // is written once and both later passes read it, so appending twice would double the list
    const bool collect_members = !struct_node->members_collected();

    // the conformance clause, now that this type's own parameters resolve - `struct Bag<E> :
    // contract::iterable<E>` names E
    if (conformance_token.has_value() && collect_members) {
        parse_conformance_clause(payload, *struct_node, conformance_snapshot, conformance_token.value());
    }

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

    // and the one a `const function` shares: `const Foo&`, the const on the *pointee*, which is
    // what makes a method that only reads reachable from a const value. built here beside the
    // mutable one so both are minted once per body and every method of the struct shares whichever
    // it asked for - and `const` on the pointee rather than on the borrow is the same shape
    // parse_value_type produces for a written `const Foo&`, so the two spellings are one type
    auto &self_const_type_node = payload.context.emplace_node<AST::TypeNode>(
        AST::ValueType::make_pointer(AST::ValueType::make_const(self_value_type), false));

    // makes a `function` in this body parse as a method: parse_funcdecl reads the struct off the
    // context to bind `$this`, prefix the owner's type parameters and register on the type. it opens
    // a null frame of its own around each body, so nothing nested inherits the receiver. both passes
    // reach a method, so both need it
    AST::SelfScope self_scope(payload.context, struct_node, &self_type_node, &self_const_type_node);

    // add the struct to the current scope, which is the list --print-ast walks. the body pass only:
    // the declaration pass has no file root, and a name is already resolvable without this because
    // parse_type_names pushed it as a namespace symbol - which is also what lets a struct be
    // recursive, and what lets a property name a struct declared further down
    if (!declarations_only) {
        payload.context.scope().add_typedecl(*struct_node);
    }

    // create an empty base scope for the properties to sit in
    auto &structscope = payload.context.emplace_node<AST::ScopeNode>();

    // an interface body admits exactly one thing: a bodyless `function` or `operator` requirement.
    // everything else is refused *here*, at the member, because this walk is the only place that knows
    // which member shape was written - and each refusal declines to publish rather than reporting on
    // the way past, so an interface never carries a property, a constructor or a destructor that a
    // later pass could find and believe in
    const bool is_interface_body = struct_node->complex_type().is_interface_kind();

    // the interface's associated types resolve throughout its body. opened for every body kind, with a
    // null owner outside an interface, so `find_type_param` has one rule rather than a conditional
    AST::AssociatedTypeScope associated_scope(
        payload.context, is_interface_body ? &struct_node->complex_type() : nullptr);

    // reports a member shape an interface cannot hold, and consumes it so the rest of the body is still
    // read. one lambda rather than the wording repeated at four arms, which is what keeps the four
    // consistent - and the shape is named rather than the token quoted, since `constructor` and a
    // property read nothing alike
    auto refuse_interface_member = [&](const TokenReference &at, const std::string &shape) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(at),
            fmt::format(
                "'{}' is an interface, so it cannot declare {}. An interface holds requirements only - "
                "a `function` or `operator` signature ending in ';'.",
                struct_node->type_name(), shape));
    };

    // and the three shapes that are refused *with a body* - a nested type, a constructor, a destructor.
    // consuming them is the half most likely to drift, since the wording was already shared and the two
    // skips were not: a copy that forgets skip_till_end_of_scope reads the refused member's own body as
    // more members and reports every statement in it
    auto refuse_braced_member = [&](const std::string &shape) {
        refuse_interface_member(cursor.current(), shape);
        cursor.skip_until({ Token::Type::t_open_brace });
        cursor.skip_till_end_of_scope();
    };

    while (!cursor.is_done()) {
        if (cursor.is_type(Token::Type::t_hash)) {
            // an attribute ahead of a member. it lands on `context.scope()` - the file root, since a
            // struct body pushes no scope of its own - and the member declaration below drains it from
            // there, exactly as a top-level declaration does. `#[core: "string_view"]` on a nested
            // `view` is the case that needs it
            parse_attribute(payload);
        }
        else if (starts_typedecl(cursor)) {
            // a nested type. recursed into rather than skipped, so one function owns "what does a
            // struct body contain" - and the recursion needs nothing passed to it: the SelfScope
            // opened above is what tells the nested call it has an owner, exactly as it tells a
            // `function` below that it is a method
            //
            // the node is *not* added as a property or a member of this struct's layout. a nested type
            // has no storage; it is a name, and add_member_type in parse_type_names is where it lives
            //
            // ahead of starts_vardecl because this is an unambiguous keyword and that one scans the
            // type grammar - the order costs nothing and means the two can never race
            if (is_interface_body) {
                refuse_braced_member("a nested type");
                continue;
            }

            parse_typedecl(payload);
        }
        // `type Iter : contract::iterator<V>;` - an interface's associated type.
        //
        // recognised contextually rather than through a `t_type` keyword token, which would break every
        // `struct type`, `function type()` and plain identifier spelled that way for no gain. the
        // `peek(1)` guard is load-bearing and not decorative: a struct named `type` can legally declare
        // a property `type $x;`, and only the `$` tells the two apart. `constructor` sets the precedent
        //
        // ahead of starts_vardecl for the nested-type arm's reason: an unambiguous contextual keyword
        // should not race the type-grammar scanner
        else if (cursor.is_type(Token::Type::t_identifier)
            && cursor.current().value() == "type"
            && cursor.peek_is_type(1, Token::Type::t_identifier)) {
            parse_associated_type(payload, *struct_node, is_interface_body, collect_members);
        }
        // **ahead of starts_vardecl**, for the two arms above's reason and one sharper than theirs:
        // a method may be written `const function get()`, and starts_vardecl reads that leading
        // `const` as the head of a property declaration. `function` is a keyword token, so nothing
        // that starts a vardecl can ever reach this arm - the order costs nothing and is what keeps
        // the modifier from racing the type-grammar scanner
        else if (starts_funcdecl(cursor)) {
            // a method. the declaration lands in the *enclosing* scope's children rather than in the
            // struct - the struct body never pushes a scope, so `payload.context.scope()` is still
            // the file/namespace root, which is the list codegen walks to emit bodies. exactly how a
            // constructor already reaches codegen; being a member is a lookup rule, not a placement
            // the declaration pass's body is parse_funcdecl's own to consume: it walks the body's
            // declaration surface so a `function` nested in a method joins its block's overload set.
            // skipping it here as well would eat the token after the body
            //
            // an interface's method needs no arm of its own: a bodyless `function f() : void;` already
            // parses and registers through register_member_function, which is all a requirement is.
            // only the *body* has to be refused, and that is asked of the returned node below - in the
            // body pass, since the declaration pass returns before it ever reaches one
            AST::FunctionDeclNode *member = parse_funcdecl(payload);

            if (is_interface_body && member != nullptr && member->body != nullptr) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(member->declaration_site_token()),
                    fmt::format(
                        "'{}' is a requirement of the interface '{}', so it cannot have a body - end it "
                        "with ';' and let each implementor write one.",
                        member->signature_description(), struct_node->type_name()));
            }
        }
        // **also ahead of starts_vardecl**, and for the sharpest version of the arm above's reason: both
        // spellings begin with `const`, and only the `$` on the name says which. Parser::starts_constdecl is
        // the one owner of that question
        else if (starts_constdecl(payload)) {
            const TokenReference at = cursor.current();

            // a requirement is behaviour. a constant is a value the owner names, so it is storage-shaped in
            // every sense that matters here - refused for the reason a property is
            if (is_interface_body) {
                refuse_interface_member(at, "a constant");
                cursor.try_skip_to_next_statement();
            }
            // the nested-type refusal's reason, exactly: the initializer may mention the owner's `T`, and a
            // constant is expanded into its use sites *before* the monomorphizer runs - so there is nothing
            // to substitute that `T` from, and no later pass that could
            else if (struct_node->is_generic()) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(at),
                    fmt::format(
                        "A constant cannot be declared inside the generic type '{}' - its value is copied to "
                        "each use site before type arguments are known. Declare it alongside its owner "
                        "instead.",
                        struct_node->type_name()));
                cursor.try_skip_to_next_statement();
            }
            // published in the declaration pass only, the idiom parse_associated_type follows: both passes
            // walk this body, and publishing twice would report the second as a redeclaration of the first
            else if (collect_members) {
                parse_constdecl(payload, struct_node);
            }
            else {
                cursor.try_skip_to_next_statement();
            }
        }
        else if (starts_vardecl(payload)) {
            auto var = parse_varexpr(payload, &structscope);

            // a requirement is behaviour, not storage. a required *property* would fix the layout of
            // every implementor and mean nothing without a stored offset, so it is refused - which also
            // keeps ComplexType::has_property_layout() true of exactly the two kinds that have one
            if (is_interface_body) {
                refuse_interface_member(
                    var != nullptr ? var->token_varname : cursor.current(), "a property");
            }
            // append the var as a property of the struct
            else if (var && collect_members) {
                struct_node->add_property(var);
            }
        }
        else if (starts_operatordecl(cursor)) {
            // **refused, but not here.** an operator is not a member of either of its operand types -
            // it is a free declaration whose symbol is global, and its precedence is one entry in one
            // table - so there is nothing for a struct body to own. handed to parse_operatordecl
            // anyway, which refuses it off Context::self_struct_ptr - live over this whole walk - and
            // consumes it, so the rest of the members are still read
            //
            // that is the reason publish_implicit_conversion refuses a free function rather than each
            // caller doing it: the declaration's own parser owes the diagnostic, and every walk that
            // reaches one then gets it. spelling it here as well made the wording a copy
            Parser::parse_operatordecl(payload);
        }
        else if (cursor.is_type(Token::Type::t_identifier) && cursor.current().value() == "constructor") {
            // an interface is never constructed - a value of one is always some implementor's object
            // widened to it - so a constructor here could not be called by anything
            if (is_interface_body) {
                refuse_braced_member("a constructor");
                continue;
            }

            parse_constructor(payload, struct_node, self_value_type);
        }
        else if (cursor.is_type(Token::Type::t_destructor)) {
            // a real token, unlike `constructor` above: it has to be one so no member can be named
            // `destructor` and collide with the mangled name of the real thing
            //
            // an interface owns no storage of its own, and what a value of one holds is torn down by
            // the concrete class's own destructor, reached through the object's typeinfo
            if (is_interface_body) {
                refuse_braced_member("a destructor");
                continue;
            }

            parse_destructor(payload, struct_node, &self_type_node);
        }
        else if (cursor.is_type(Token::Type::t_close_brace)) {
            cursor.skip();
            break;
        }

        else {
            payload.collect_unexpected_token(Token::Type::t_unknown);

            // when we encounter an unexpected token, we skip until we find a semicolon or a brace
            // in the hopes that there is    simply a typo in the code or something minor that we can recover
            // from we might have to skip till the end of the scope otherwise..
            cursor.skip(); // always skip the token causing the issue
            cursor.skip_until({ Token::Type::t_semicolon, Token::Type::t_open_brace, Token::Type::t_close_brace });

            // if we find a semicolon or a close brace, we skip it in the hopes that afterwards we can
            // continue parsing
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
    // never for an interface: it has no fields to take, so what would be synthesized is a zero-argument
    // constructor of a type that is never constructed - and unlike the refusals in the body walk above
    // this one nobody wrote, so there would be no token to report it at
    if (collect_members && !is_interface_body) {
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
