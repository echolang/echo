#include "Parser/FuncDeclParser.h"

#include "AST/VarDeclNode.h"
#include "AST/TypeNode.h"
#include "AST/FunctionDeclNode.h"
#include "AST/LiteralValueNode.h"
#include "AST/TypeDeclNode.h"
#include "AST/ASTArgumentFit.h"
#include "AST/ASTBuiltin.h"
#include "AST/ASTDestruction.h"
#include "AST/ASTMemberLookup.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/AttributeNode.h"
#include "AST/ExprNode.h"

#include "Parser/CaptureParser.h"
#include "Parser/TypeParser.h"
#include "Parser/ExprParser.h"
#include "Parser/VarDeclParser.h"
#include "Parser/ScopeParser.h"
#include "Parser/SymbolParser.h"
#include "Parser/AttributeParser.h"
#include "Token.h"

#include <fmt/core.h>

#include <algorithm>
#include <optional>
#include <vector>

void Parser::push_implicit_param(
    Parser::Payload &payload,
    AST::FunctionDeclNode &decl,
    AST::ScopeNode &into,
    const std::string &name,
    AST::TypeNode *type_node,
    const TokenReference &at
)
{
    auto token = payload.context.make_virtual_token(name, Token::Type::t_varname, at);
    auto *vardecl = payload.context.emplace_nodep<AST::VarDeclNode>(token, type_node);

    // ahead of everything the caller wrote. FunctionDeclNode::clone clones args before the body
    // precisely so the body's references rebind to the cloned declaration
    decl.args.push_back(vardecl);

    // declared in the argument scope, not in the body, so it resolves the same way every other
    // parameter does. the argument scope's children are never emitted - the body is a separate child
    // scope and codegen allocas from `args` - so this costs no second slot, and it is what keeps
    // StmtCodegen::gen_scope's sweep over a scope's declarations off the parameters: gen_scope only ever
    // runs on a body or a block, never on an argument scope, so a parameter is seated exactly once
    into.add_vardecl(*vardecl);
}

bool Parser::parse_function_body(
    Parser::Payload &payload,
    AST::FunctionDeclNode &decl,
    AST::ScopeNode &scope,
    AST::ClosureExprNode *closure
)
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

        decl.body = &parse_scope(payload, body_brace);
    }

    if (!payload.expect_token(Token::Type::t_close_brace)) {
        return false;
    }

    cursor.skip(); // the closing brace

    payload.context.pop_scope();

    return true;
}

bool Parser::starts_access_effect(Parser::Cursor &cursor, AST::AccessEffect &effect)
{
    if (!cursor.is_type(Token::Type::t_identifier)) {
        return false;
    }

    // an identifier followed by `$name` is that parameter's type, whatever it spells
    if (cursor.peek_is_type(1, Token::Type::t_varname)) {
        return false;
    }

    const std::string &word = cursor.current().value();

    if (word == "read") {
        effect = AST::AccessEffect::t_read;
    }
    else if (word == "inout") {
        effect = AST::AccessEffect::t_inout;
    }
    else if (word == "out") {
        effect = AST::AccessEffect::t_out;
    }
    else {
        return false;
    }

    return true;
}

bool Parser::parse_parameter_list(
    Parser::Payload &payload,
    AST::FunctionDeclNode &decl,
    AST::ScopeNode &into,
    const TokenReference &report_at,
    Token::Type closing
)
{
    auto &cursor = payload.cursor;

    while (!cursor.is_type(closing)) {
        if (cursor.is_done()) {
            payload.collector.collect_issue<AST::Issue::UnexpectedToken>(
                payload.context.code_ref(report_at), closing, Token::Type::t_unknown);
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

        // `read slice<T> $src` - what this parameter does to the storage its argument names. read
        // here for takes_ownership's reason and asked after it, so `mv` keeps being the one spelling
        // of a consuming parameter rather than becoming a fourth effect keyword beside it
        AST::AccessEffect access_effect = AST::AccessEffect::t_none;
        if (starts_access_effect(cursor, access_effect)) {
            cursor.skip();
        }

        // `forEvent: string $name` - the call site must use this label. read here rather than in
        // parse_varexpr because a label is a property of a parameter, and `identifier :` is not
        // a type. peek past the colon so `Foo $x` stays a type and a name
        std::optional<TokenReference> label;
        if (cursor.is_type(Token::Type::t_identifier)
            && cursor.peek_is_type(1, Token::Type::t_colon)) {
            label.emplace(cursor.current());
            cursor.skip();
            cursor.skip();
        }

        auto *param = parse_varexpr(payload, &into);

        if (param != nullptr) {
            param->takes_ownership = takes_ownership;
            param->access_effect = access_effect;
            if (label.has_value()) {
                param->label_token.emplace(label.value());
            }

            if (label.has_value()) {
                for (const AST::VarDeclNode *earlier : decl.args) {
                    if (earlier != nullptr && earlier->has_label() && earlier->label() == label->value()) {
                        payload.collector.collect_issue<AST::Issue::DuplicateParameterLabel>(
                            payload.context.code_ref(label.value()),
                            fmt::format(
                                "The label '{}' is already used on parameter '{}'.",
                                label->value(),
                                earlier->name_full()));
                        break;
                    }
                }
            }
        }

        decl.args.push_back(param);
    }

    // skip the closing token
    cursor.skip();

    return true;
}

bool Parser::starts_funcdecl(Parser::Cursor &cursor)
{
    // the member modifier prefix, one keyword of each kind. it is *optional* here rather than a
    // second predicate so a struct body has one arm for "this is a method" however it was written -
    // and so `const` at the head of a property declaration, which reaches the same loop, is told
    // apart by the `function` behind it rather than by an ordering the two arms have to agree on
    //
    // the prefix is *walked* rather than enumerated, which is what keeps this predicate and the loop
    // in parse_funcdecl that consumes it accepting the same token runs. both orders reach here even
    // though `const static` is refused there, and deliberately: a predicate that did not recognise it
    // would send the declaration to the *property* scanner, and the refusal a reader gets would be
    // about a type name rather than about the two modifiers they wrote
    size_t offset = 0;

    while (cursor.peek_is_type(offset, Token::Type::t_static)
        || cursor.peek_is_type(offset, Token::Type::t_const)) {
        offset++;
    }

    if (!cursor.peek_is_type(offset, Token::Type::t_function)) {
        return false;
    }

    const size_t name_offset = offset + 1;

    if (cursor.peek_is_type(name_offset, Token::Type::t_identifier)) {
        return true;
    }

    // a closure, a callable type, a capture list - not a declaration
    if (cursor.peek_is_type(name_offset, Token::Type::t_open_paren)
        || cursor.peek_is_type(name_offset, Token::Type::t_open_angle)
        || cursor.peek_is_type(name_offset, Token::Type::t_open_bracket)) {
        return false;
    }

    // a reserved word sitting where the name should be. treating it as "not a declaration" sent the
    // member walk to its unexpected-token arm, which blamed `static` and recovered without matching
    // braces - silent 139 in a multi-file module. enter parse_funcdecl so it can refuse at this token
    // and skip_refused_function can consume the body
    if (!cursor.is_valid_offset(name_offset)) {
        return false;
    }

    return token_spells_a_word(cursor.peek(name_offset).value());
}

bool Parser::starts_closure_literal(Parser::Cursor &cursor)
{
    if (!cursor.is_type(Token::Type::t_function)) {
        return false;
    }

    // `function (` - no list. `function [` - the closed capture list, then `(`. `function <`
    // is the callable type and `function ident` is a declaration; neither is an expression
    return cursor.peek_is_type(1, Token::Type::t_open_paren)
        || cursor.peek_is_type(1, Token::Type::t_open_bracket);
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

void Parser::skip_refused_function(Parser::Payload &payload)
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
    const TokenReference &at
)
{
    auto &env_type = payload.context.emplace_node<AST::TypeNode>(AST::ValueType::make_class(environment));

    Parser::push_implicit_param(payload, decl, into, "$__env", &env_type, at);
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

    std::optional<std::vector<AST::ClosureExprNode::Capture>> capture_list;

    if (cursor.is_type(Token::Type::t_open_bracket)) {
        std::vector<AST::ClosureExprNode::Capture> captures;

        if (!Parser::parse_capture_list(payload, captures)) {
            return nullptr;
        }

        capture_list = std::move(captures);
    }

    // a name nobody can spell, discriminated so two closures never share a symbol - including two written
    // inside one block, which the enclosing lexical namespace cannot tell apart.
    //
    // the *position* rather than a counter, and this one carries a second reason beyond reproducibility:
    // the environment type is minted as `<this name>.env`, and its identity, its typeinfo global and its
    // release thunk all rest on this string alone being unique
    auto name_token = payload.context.make_virtual_token(
        fmt::format("closure${}", payload.context.site_discriminator(function_token)),
        Token::Type::t_identifier,
        function_token);

    auto *closure_decl = payload.context.emplace_nodep<AST::FunctionDeclNode>(name_token, function_token);

    closure_decl->is_closure = true;

    // the block it was written in, which is what makes its symbol unique per block and lets a diagnostic
    // say which function it appeared in. deliberately *not* registered in the FunctionRegistry: no name
    // reaches a closure, so it belongs to no overload set
    closure_decl->ast_namespace = payload.context.current_namespace;

    // and where it was written, which is what the body inside it is judged against: a call to a `private`
    // declaration made from a closure written in the same file has to be allowed, and TypeChecker asks the
    // enclosing declaration for that. its visibility stays `t_public` and has to - no name reaches a
    // closure, so there is nothing to hide it from
    closure_decl->declared_in = AST::origin_at(payload.context);

    // inherit the enclosing function's type parameters, the way a method inherits its
    // owner's: so TypeChecker skips the template closure (it still names T) and the
    // monomorphizer clones a concrete body per instance. a nested `function` stays
    // refused - it is not cloned with its parent
    if (AST::FunctionDeclNode *enclosing = payload.context.current_function_ptr) {
        closure_decl->type_parameters = enclosing->type_parameters;
        closure_decl->inherited_type_param_count = closure_decl->type_parameters.size();
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
        payload.context.current_namespace);

    // `args[0]`, ahead of everything the user wrote - which is where capture_variable reads it back from
    push_environment_param(payload, *closure_decl, closure_scope, environment, function_token);

    if (!payload.expect_token(Token::Type::t_open_paren)) {
        return nullptr;
    }

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

    // stamped before the body: capture_variable reads the closed set. the enclosing nest is
    // on the context, which FunctionBodyScope pushes this node onto once the body starts
    closure_expr.capture_list = std::move(capture_list);

    if (!parse_function_body(payload, *closure_decl, closure_scope, &closure_expr)) {
        return nullptr;
    }

    Parser::report_unused_captures(payload, closure_expr, *environment);

    // to the file root, like every other declaration: codegen emits bodies from there and
    // AST::OwnershipPass resolves drops from the same list. skipped when the enclosing parse
    // will not keep this tree - an instance property default is cloned into constructors, and
    // that clone is what AST::prepend_property_defaults publishes
    if (payload.context.publish_declarations) {
        payload.context.declaration_scope().add_funcdecl(*closure_decl);
    }

    // only now is it known whether anything was captured. an environment with no properties is left off
    // the node entirely, so the closure's env slot stays null and nothing is allocated for it
    if (environment->property_count() > 0) {
        closure_expr.environment_type = environment;
    }

    return &closure_expr;
}

void Parser::drain_attributes(Parser::Payload &payload, AST::AttributeList &into)
{
    // each pass emplaces its own AttributeNode over the same tokens, so a declaration reached twice
    // accumulates two per attribute. that is already true of a TypeDeclNode, which drains in both
    // passes, and AttributeList::get_first is what makes it not matter
    for (auto &attr : payload.context.scope().collect_attributes()) {
        into.push_back(attr);
    }
}

void Parser::publish_declaration_markers(
    Parser::Payload &payload,
    AST::FunctionDeclNode *funcdecl,
    AST::TypeDeclNode *owner_struct,
    const TokenReference &nametoken
)
{
    publish_implicit_conversion(payload, funcdecl, owner_struct, nametoken);

    // no validation here on purpose, unlike `#[implicit]`. The attribute is meaningless rather than wrong
    // on a declaration with no body of ours - an extern, an intrinsic, a builtin - and the stdlib stacks it
    // on every intrinsic in std/math/intrinsics.eco already. AST::function_emission_kind answers those before
    // it ever looks at this flag, so the combination costs nothing and refusing it would break that file
    funcdecl->is_inline = funcdecl->attributes.get_first("inline") != nullptr;
}

// publishes a method or static marked `#[implicit]` as one of its owner's implicit conversions, and
// reports every way of marking something that cannot be one. the contract, and why it is here rather
// than on FunctionRegistry, are in the header
//
// this is publish_copy_constructor's situation exactly - a second fact about an already-registered
// declaration - and it is that function's shape, down to comparing the slot by identity so the body
// pass revisiting the declaration pass's node is not a redeclaration
//
// every refusal declines to *publish*, so AST::find_implicit_conversion never has to re-filter what
// it finds. each one is reported in both passes, and stays one diagnostic because
// Collector::collect_issue de-duplicates on (kind, token range, message) - both passes locate on the
// same attribute tokens, which is the only thing about running twice that is not free
void Parser::publish_implicit_conversion(
    Parser::Payload &payload,
    AST::FunctionDeclNode *funcdecl,
    AST::TypeDeclNode *owner_struct,
    const TokenReference &nametoken
)
{
    AST::AttributeNode *implicit_attr = funcdecl->attributes.get_first("implicit");

    if (implicit_attr == nullptr) {
        return;
    }

    const auto report = [&](const std::string &message) {
        payload.collector.collect_issue<AST::Issue::InvalidImplicitConversion>(
            payload.context.code_ref(implicit_attr->attribute_tokens), message);
    };

    // a free function converts nothing: which type the conversion is *from* is the type it is
    // declared on, and there is no other place that answer could come from
    if (owner_struct == nullptr) {
        report(fmt::format(
            "Only a method can declare an implicit conversion - '{}' is a free function, and a "
            "conversion converts the type it is declared on.",
            nametoken.value()));
        return;
    }

    // a constructor builds its own type and a destructor tears one down; neither converts anything,
    // and neither is reachable from an argument position. asked of member_kind rather than of the
    // call site's spelling, so the two struct-member parsers do not each carry a copy of the rule.
    // a static method is the inbound shape and is admitted below
    if (funcdecl->member_kind != AST::MemberKind::t_method
        && funcdecl->member_kind != AST::MemberKind::t_static_method) {
        report(fmt::format(
            "Only a method can declare an implicit conversion - a {} cannot be one.",
            funcdecl->is_constructor() ? "constructor" : "destructor"));
        return;
    }

    const AST::ValueType target = funcdecl->get_return_type();

    // an unknown or still-generic return type was already reported where it was written, and
    // AST::argument_fit answers t_undetermined for it - so there is nothing here to say that would
    // not be a second diagnostic for one mistake
    if (target.is_unknown() || target.is_void()) {
        return;
    }

    // the whole conversion rule compares the declared return type with the parameter type, exactly.
    // on a generic owner the walk reaches the *template*, whose return type still mentions the
    // owner's `T`, and that never equals a concrete `slice<int32>` - so the declaration would be
    // accepted and then silently never fire. the walk reaches the template because an instantiation
    // holds no `_methods` of its own and reaches the template's through the template_or_self redirect -
    // the same reason AST::find_member_functions redirects an instantiation through `template_ref` - so
    // an instantiation's conversions hold declarations whose return type still mentions `T`, wherever
    // the comparison is made. answering the concrete target means substituting that return type per
    // instantiation. refuse the case rather than ship a marker that does nothing
    if (owner_struct->is_generic() || AST::contains_type_param(target)) {
        report(fmt::format(
            "'{}' is generic, and an implicit conversion on a generic type is not supported yet - "
            "the target type would have to be substituted per instantiation.",
            owner_struct->type_name()));
        return;
    }

    // a declared type, because the conversion is lowered as a call to this very method and the
    // parameter it answers is compared by type identity. deliberately not widened to let a struct
    // convert to a primitive: that would make every arithmetic site a conversion candidate, which is
    // a different rule and would want a different owner
    if (!target.has_complex_type()) {
        report(fmt::format(
            "An implicit conversion must return a declared type - '{}' is not one.",
            target.get_type_desciption()));
        return;
    }

    // an interface is a declared type, and is refused all the same: a value reaches an interface-typed
    // destination by *widening*, which the compiler already knows how to rank and lower. accepting a
    // marker here would give one conversion two routes with different ranks, and which one fired would
    // depend on nothing the author wrote. conformance is declared on the type, not per method
    if (target.is_interface()) {
        report(fmt::format(
            "An implicit conversion cannot return the interface '{}' - declare the conformance on the "
            "type instead, and a value of it widens on its own.",
            target.get_type_desciption()));
        return;
    }

    AST::ComplexType &owner = owner_struct->complex_type();

    // compared by identity, so the body pass arriving at the node the declaration pass published is
    // not a second declaration. this is the question the slot answers that the lookup does not - "is
    // this declaration already here" rather than "what converts to that type"
    if (std::ranges::find(owner.implicit_conversions(), funcdecl) != owner.implicit_conversions().end()) {
        return;
    }

    if (funcdecl->member_kind == AST::MemberKind::t_static_method) {
        // inbound: a static of the destination, one parameter, return type exactly the owner.
        // the conversion *constructs* the destination, so a class that allocates is allowed -
        // owns-nothing is an outbound rule about a window appearing in an argument list
        if (funcdecl->args.size() != 1) {
            report(fmt::format(
                "An inbound conversion takes one parameter - the source. '{}::{}' declares {}.",
                owner_struct->type_name(), nametoken.value(), funcdecl->args.size()));
            return;
        }

        if (!(target == owner_struct->value_type())) {
            report(fmt::format(
                "An inbound conversion must return '{}' - the type it is declared on.",
                owner_struct->type_name()));
            return;
        }

        const AST::ValueType source = funcdecl->parameter_type(0);

        // by value, so find_inbound_implicit_conversion stays `parameter_type(0) == from`. a
        // borrow would force the lookup to re-rank, and implicit_conversion_source already
        // peels a T& argument before the lookup is asked
        if (AST::parameter_auto_borrows(source)) {
            report(fmt::format(
                "An inbound conversion takes the source by value - '{}::{}' takes '{}'.",
                owner_struct->type_name(), nametoken.value(), source.get_type_desciption()));
            return;
        }

        // asked through the lookup rather than by re-walking the slot, so "which conversion does
        // this type have from that source" stays one rule
        if (AST::find_inbound_implicit_conversion(&owner, source) != nullptr) {
            report(fmt::format(
                "'{}' already declares an implicit conversion from '{}'.",
                owner_struct->type_name(), source.get_type_desciption()));
            return;
        }

        owner.add_implicit_conversion(funcdecl);
        return;
    }

    // outbound: the receiver and nothing else. a `view(usize, usize)` beside it is a substring
    // accessor - a different operation on the same name - and nothing would pass it arguments at
    // a call site the user did not write
    if (funcdecl->args.size() != funcdecl->implicit_arg_count()) {
        report(fmt::format(
            "An implicit conversion takes no parameters - '{}::{}' declares {}.",
            owner_struct->type_name(), nametoken.value(),
            funcdecl->args.size() - funcdecl->implicit_arg_count()));
        return;
    }

    // a conversion to its own type can never fire: AST::argument_fit answers t_exact for an identical
    // type long before it reaches the conversion arm. so this is a marker that does nothing, which is
    // the thing this whole spelling exists to stop being possible
    if (target == owner_struct->value_type()) {
        report(fmt::format(
            "An implicit conversion must return a type other than '{}'.",
            owner_struct->type_name()));
        return;
    }

    // the conversion fires at a call site nobody wrote, so what it hands over must cost nothing to
    // hand over. a return value that has to be destroyed means an allocation or a retain appearing
    // out of an argument list - which is the property `tests_eco/strings/view_conversion.test` pins
    // with `CHECK-NOT: strong.inc`. nothing would leak (a callee destroys an owning by-value
    // parameter), so this is a rule about what belongs in an invisible conversion, not a fix.
    // inbound is the other direction and does not owe this: it constructs the destination
    if (AST::needs_destruction(target)) {
        report(fmt::format(
            "An implicit conversion must return a value that owns nothing - '{}' has to be "
            "destroyed. Write the call out instead.",
            target.get_type_desciption()));
        return;
    }

    // and a *different* declaration converting to the same target does need saying: nothing would
    // decide which of them an invisible conversion meant. asked through the directed lookup rather
    // than find_implicit_conversion, which now also walks the *target* for inbound and would refuse
    // a legal outbound the moment the other type declared the reverse
    if (AST::find_outbound_implicit_conversion(&owner, target) != nullptr) {
        report(fmt::format(
            "'{}' already declares an implicit conversion to '{}'.",
            owner_struct->type_name(), target.get_type_desciption()));
        return;
    }

    owner.add_implicit_conversion(funcdecl);
}

AST::FunctionDeclNode * Parser::parse_funcdecl(
    Parser::Payload &payload,
    Parser::FuncDeclKind kind,
    Parser::VisibilityPrefix visibility
)
{
    auto &cursor = payload.cursor;

    // the declaration pass stops once the signature is registered; the body pass carries on into the
    // body. read off the payload rather than taken as an argument, so no caller in between has to
    // know which pass it is forwarding
    const bool symbol_only = payload.pass == Pass::t_declarations;

    // the member modifier prefix. `const` is read here, before the `function` keyword, because that is
    // where starts_funcdecl already looks; the visibility arrived from the dispatch that got here, which
    // had to consume it before it could tell a method from a property - see MemberModifiers
    MemberModifiers modifiers { visibility, std::nullopt, std::nullopt };

    // both orders, because starts_funcdecl accepts both - a refusal about the pair is worth more than
    // one about whichever of them happened to be second
    while (cursor.is_type({ Token::Type::t_const, Token::Type::t_static })) {
        if (cursor.is_type(Token::Type::t_static)) {
            modifiers.static_token.emplace(cursor.current());
        }
        else {
            modifiers.const_token.emplace(cursor.current());
        }

        cursor.skip();
    }

    if (!cursor.is_type(Token::Type::t_function)) {
        payload.collect_unexpected_token(Token::Type::t_function);
        cursor.try_skip_to_next_statement();
        return nullptr;
    }

    // skip the function keyword
    cursor.skip();

    // next token should be an identifier aka the function name. a reserved word here is the one
    // starts_funcdecl now admits so this arm can name it: `'null' is reserved and cannot be a method
    // name`. skip_refused_function, not try_skip_to_next_statement - a body is full of `;` and `}`
    if (!cursor.is_type(Token::Type::t_identifier)) {
        if (!cursor.is_done() && token_spells_a_word(cursor.current().value())) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(cursor.current()),
                fmt::format(
                    "'{}' is reserved and cannot be a {} name",
                    cursor.current().value(),
                    payload.context.self_struct_ptr != nullptr ? "method" : "function"));
        }
        else {
            payload.collect_unexpected_token(Token::Type::t_identifier);
        }

        Parser::skip_refused_function(payload);
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
                Parser::skip_refused_function(payload);
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
        Parser::skip_refused_function(payload);
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

    // who may call it, and where it was written to answer that against. written in both passes, which is
    // safe by construction: they read the same tokens at the same index, so the second computes what the
    // first already recorded - the same standing every other field reconciled on a declaration site has
    funcdecl->visibility = modifiers.visibility.value;
    funcdecl->declared_in = AST::origin_at(payload.context);

    // a `function` written inside a `{ }` block is a scoped declaration - the lexical namespace above
    // is the block's - and it has no access to the enclosing frame. an enclosing *type parameter* is
    // exactly such an access: `T` would resolve through the type-param scope stack and silently make
    // this declaration depend on a substitution nothing will ever hand it, so it is rejected outright
    // rather than lowered against an unresolved generic. lifted once closures can be generic
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
        Parser::skip_refused_function(payload);
        return nullptr;
    }

    // a `function` written inside a struct body is a method: the enclosing struct arrived on the
    // context, and it is the only thing that distinguishes this from a free function
    AST::TypeDeclNode *owner_struct = payload.context.self_struct_ptr;
    AST::TypeNode *self_type = payload.context.receiver_type(modifiers.is_const());

    // **two questions, not one**, ever since `static function` existed. is_owned decides what carries
    // the declaration - the owner's method table, the mangled name's owner segment, the inherited type
    // parameters. has_receiver decides whether a `$this` is pushed ahead of what the user wrote.
    //
    // they were one predicate while every function written in a struct body had a receiver, and the
    // sites below that need the second are exactly the ones a static would silently break - see
    // FunctionDeclNode::has_receiver, which is this same split on the declaration
    const bool is_owned = owner_struct != nullptr;
    const bool has_receiver = is_owned && self_type != nullptr && !modifiers.is_static();

    // `static` is a member modifier: there is no type for the function to belong to at file scope,
    // and no receiver to have suppressed. reported and then ignored, so the declaration still
    // registers as the free function it was written as and calls to it get ordinary diagnostics
    if (modifiers.is_static() && !is_owned) {
        payload.collector.collect_issue<AST::Issue::StaticOutsideType>(
            payload.context.code_ref(modifiers.static_token.value()),
            nametoken.value()
        );

        modifiers.static_token.reset();
    }

    // `const` says what a receiver may do, and a static has none - so the pair is not a redundancy to
    // drop but a question about a parameter that is not there. reported at the `const`, which is the
    // modifier that does not apply, and then dropped so the declaration still registers as the static
    // it was written as
    //
    // asked here rather than where the two were read, so the message can name the function: at the
    // modifiers the cursor has not reached the name yet. `has_receiver` above already accounts for it
    if (modifiers.is_const() && modifiers.is_static()) {
        payload.collector.collect_issue<AST::Issue::ConstOnStaticFunction>(
            payload.context.code_ref(modifiers.const_token.value()),
            nametoken.value()
        );

        modifiers.const_token.reset();
    }

    // `const` qualifies a *receiver*, so there is nothing for it to say about a free function or an
    // extern one - their parameters carry their own const, where the caller can see it. reported and
    // then ignored rather than refused outright, so the declaration still registers and every call
    // to it gets an ordinary diagnostic instead of "unknown function"
    //
    // no reset is needed to ignore it: outside a struct body there is no const receiver
    // node to have bound, which is what `has_receiver` being false above already says
    if (modifiers.is_const() && !has_receiver) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(modifiers.const_token.value()),
            fmt::format(
                "'{}' is not a method, so it cannot be declared const - only a receiver is what "
                "`const` would qualify. Write `const` on the parameters that are read-only instead.",
                nametoken.value()));
    }

    // a method of a generic struct carries the owner's parameters ahead of its own, so that one
    // TypeSubstitution binds both: the owner's T from the receiver argument, its own U from the rest
    // declare_type_parameters owns that shape - what is shared, what is re-declared, and where the
    // split falls
    //
    // is_owned rather than has_receiver: a **static** of a generic owner carries them too, and has to.
    // `result<T, E>::ok` names T and E in its signature and its body with no receiver to bind them
    // from - the call site's owner is what binds them instead, through AST::static_owner_bindings
    std::vector<AST::TypeParamDecl *> inherited_params;
    if (is_owned && owner_struct->is_generic()) {
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
    if (has_receiver) {
        funcdecl->member_kind = AST::MemberKind::t_method;
        push_receiver_param(payload, *funcdecl, funcscope, self_type, nametoken);
    }
    else if (modifiers.is_static()) {
        // the whole of what `static` does to the shape: the kind is set and no receiver is pushed, so
        // args[0] is whatever the user wrote first. everything else about the declaration - the owner,
        // the inherited type parameters, the mangled name - is a method's
        funcdecl->member_kind = AST::MemberKind::t_static_method;
    }

    // parse the function arguments
    if (!parse_parameter_list(payload, *funcdecl, funcscope, nametoken)) {
        return nullptr;
    }

    // next token should be ":" for the return type
    if (!cursor.is_type(Token::Type::t_colon)) {
        payload.collect_unexpected_token(Token::Type::t_colon);
        Parser::skip_refused_function(payload);
        return nullptr;
    }

    // skip the colon
    cursor.skip();

    // parse the return type
    if (!can_parse_type(payload)) {
        payload.collect_unexpected_token(Token::Type::t_identifier);
        Parser::skip_refused_function(payload);
        return nullptr;
    }

    funcdecl->return_type = parse_type(payload);

    // **here rather than after the body**: the return type one line
    // up is the last thing an attribute could have something to say about, and `#[implicit]` below
    // needs the whole signature. it also has to run in *both* passes - the declaration pass returns
    // further down without ever reaching a body - and that is what fixes a leak rather than works
    // around one: an attribute a method left on the stack was drained by the next `struct` to come
    // along (TypeDeclParser's own collect_attributes), landing on a type declaration that never
    // asked for it
    Parser::drain_attributes(payload, funcdecl->attributes);

    // the signature is complete, so this is the earliest point the declaration can join its
    // overload set. registering in *both* passes is intentional and cheap: the symbol pass makes
    // the declaration visible to calls written above it and in other files, and the full pass
    // finds its own declaration site already present and returns the same handle
    if (is_owned) {
        // a method joins its owner's method table instead, so it is reachable through a receiver
        // and not as a free function of the enclosing namespace. owner_type is what tells the
        // mangler and every diagnostic that the first parameter is not one the user wrote
        funcdecl->owner_type = &owner_struct->complex_type();

        // **two tables, because there are two ways to reach one and they must not overlap.** a static
        // is reached by naming its type and never through a receiver, so putting it in the method
        // table would make `$box->make(1)` resolve - a call whose args[0] the callee never declared
        if (funcdecl->is_static_method()) {
            payload.collector.functions.register_static_function(
                payload.collector, payload.context.code_ref(nametoken), funcdecl, owner_struct->complex_type());
        }
        else {
            payload.collector.functions.register_member_function(
                payload.collector, payload.context.code_ref(nametoken), funcdecl, owner_struct->complex_type());
        }
    }
    else {
        payload.collector.functions.register_function(
            payload.collector, payload.context.code_ref(nametoken), funcdecl);
    }

    // ...and then, of an already-registered declaration, the second fact `#[implicit]` states about
    // it. outside the branch above rather than in its method arm, because "only a method can declare
    // one" is a diagnostic this owes and a helper called only for methods could never report it
    publish_declaration_markers(payload, funcdecl, owner_struct, nametoken);

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
            payload.collector.collect_issue<AST::Issue::ExternGeneric>(
                payload.context.code_ref(nametoken),
                "An extern function cannot be generic - a single C symbol has no per-instantiation body");
            Parser::skip_refused_function(payload);
            return nullptr;
        }

        // the body lives in another object file. a body here would be compiled under the raw
        // symbol and collide with the real definition at link time
        if (!cursor.is_type(Token::Type::t_semicolon)) {
            payload.collector.collect_issue<AST::Issue::ExternHasBody>(
                payload.context.code_ref(nametoken),
                "An extern function declaration cannot have a body - it must end with ';'");
            Parser::skip_refused_function(payload);
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
        // a **bodyless** declaration's terminator, consumed in this pass too - the same reason the
        // extern arm above consumes its own: a declaration's tail belongs to the parser that knows the
        // declaration's shape, and leaving it means the caller's walk meets a stray ';'. at file scope
        // that was silently tolerated, so only an interface requirement in a body walk surfaced it
        else if (cursor.is_type(Token::Type::t_semicolon)) {
            cursor.skip();
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

    // if next token is a semicolon we are done for now
    if (cursor.is_type(Token::Type::t_semicolon)) {
        cursor.skip();

        // a bodyless declaration gets its implementation from one of two places: an LLVM
        // intrinsic, which still becomes a real llvm::Function, or a compiler builtin, which has
        // no symbol at all and whose call sites fold to a constant
        //
        // an intrinsic is spelled as a **string** because an LLVM intrinsic name is free text with
        // dots in it; a builtin is spelled as a **name**, because the set is closed and a closed
        // vocabulary reads bare - the same rule `#[if: os == darwin]` has always followed
        if (auto *intrinsic_attr = funcdecl->attributes.get_first("intrinsic")) {
            funcdecl->intrinsic = read_attribute_value(payload, intrinsic_attr, "intrinsic",
                [](AST::AttributeReader &reader, const AST::AttributeValue &written) {
                    return reader.string(written);
                });

            if (!funcdecl->intrinsic) {
                return nullptr;
            }
        }

        if (auto *builtin_attr = funcdecl->attributes.get_first("builtin")) {
            funcdecl->builtin = read_attribute_value(payload, builtin_attr, "builtin",
                [](AST::AttributeReader &reader, const AST::AttributeValue &written) {
                    std::optional<std::string> value = reader.name(written);

                    if (value.has_value() && !AST::is_known_builtin(value.value())) {
                        reader.refuse(written.span,
                            fmt::format("Unknown compiler builtin '{}'.", value.value()));
                        value.reset();
                    }

                    return value;
                });

            if (!funcdecl->builtin) {
                return nullptr;
            }
        }

        return funcdecl;
    }

    if (!parse_function_body(payload, *funcdecl, funcscope)) {
        return nullptr;
    }

    return funcdecl;
}
