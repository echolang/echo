#ifndef FUNCDECLPARSER_H
#define FUNCDECLPARSER_H

#pragma once

#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/ASTContext.h"
#include "AST/FunctionDeclNode.h"
#include "Parser/ParserPayload.h"

namespace AST
{
    class ClosureExprNode;
    class TypeDeclNode;
};

namespace Parser
{
    // moves the attributes staged on the current scope onto `into`, so the declaration they were
    // written ahead of is the one that consumes them.
    //
    // shared by every declaration that can carry one - a function, a method, a constructor, a
    // destructor, a struct - because a declaration that does *not* drain leaves its attributes on the
    // stack for whatever comes along next, which is how a method's `#[implicit]` used to land on the
    // following `struct`. taken as an AttributeList rather than a FunctionDeclNode for exactly that
    // reason: the struct site is the other half of that bug and drains through here too. called once
    // the declaration node is in hand, which is also the earliest point at which an attribute has
    // something to be read against
    void drain_attributes(Payload &payload, AST::AttributeList &into);

    // publishes every marker a member declaration's attributes state about it, or reports why the
    // declaration cannot carry one.
    //
    // the one list of "what does an attribute publish about a member", so a second marker - whatever
    // todo/A18 brings - is added here and reaches the method, constructor and destructor sites at
    // once instead of being remembered at three. the drain stays at each site, because a declaration
    // reads `#[intrinsic]`/`#[builtin]` off its own list before it gets here
    void publish_declaration_markers(
        Payload &payload,
        AST::FunctionDeclNode *funcdecl,
        AST::TypeDeclNode *owner_struct,
        const TokenReference &nametoken);

    // publishes `#[implicit]` on `owner_struct`, or reports why the declaration cannot carry it.
    //
    // the one owner of the marker: what it means, which shapes are refused, and how each refusal
    // reads. every declaration that can be written with one reaches it through
    // publish_declaration_markers - a free function and a constructor or destructor only so that they
    // can be told they cannot be one, which is a diagnostic no method-only helper could ever report
    //
    // a second fact about an *already-registered* declaration, so it is called after registration and
    // deliberately not on FunctionRegistry: all three registrars return early on the body pass, and a
    // fourth would publish in whichever pass happened to claim the declaration site first
    void publish_implicit_conversion(
        Payload &payload,
        AST::FunctionDeclNode *funcdecl,
        AST::TypeDeclNode *owner_struct,
        const TokenReference &nametoken);

    // which grammar a `function` declaration is being read under. an extern declaration accepts
    // the `function <c_symbol> as <echo_name>(...)` renaming spelling and must be bodyless;
    // everything else about the signature is parsed by the same code, so the two forms cannot
    // drift apart
    enum class FuncDeclKind
    {
        t_normal,
        t_extern,
    };

    // whether the body is parsed or left for the body pass is read off `payload.pass`
    AST::FunctionDeclNode *parse_funcdecl(Payload &payload, FuncDeclKind kind = FuncDeclKind::t_normal);

    // consumes a declaration's body from its first token: either a braced body or the bare `;` of a
    // declaration that has none. the one place that knows how a declaration body is skipped, shared by
    // the pass that does not read member bodies and by the recovery for a refused `function`
    //
    // brace-depth aware rather than token-by-token, because a body's closing brace would otherwise read
    // as the end of the enclosing scope - silently truncating a struct and losing every member written
    // after it, or resuming *inside* a body full of semicolons and reporting a cascade of nonsense
    void skip_declaration_body(Payload &payload);

    // does a `function` *declaration* start here? the keyword alone no longer answers it: `function`
    // introduces three different things, told apart by the one token after it -
    //
    //   function <ident> (...)   a declaration
    //   function < ... >         the callable type `function<R(P...)>`
    //   function ( ... ) { }     a closure literal, an expression
    //
    // the sole owner of that question, so the statement dispatch, the declaration-surface walk and the
    // struct member walk cannot come to three different answers
    bool starts_funcdecl(Parser::Cursor &cursor);

    // does a closure literal start here? `function (` - the third of the three things the keyword
    // introduces, and the only one that is an expression
    bool starts_closure_literal(Parser::Cursor &cursor);

    // `function(int32 $a) : int32 { ... }` in a value position.
    //
    // the same machinery a declaration uses - one parameter list parser, one body parser - over an
    // *anonymous* declaration that is hoisted to the file root and entered in no overload set. its
    // `args[0]` is the environment its captures will live in, exactly the way a method's is its receiver
    AST::ClosureExprNode *parse_closure_literal(Payload &payload);

    // captures `vardecl` into the closure currently being parsed and answers the expression its body
    // should read instead: a member access on the environment parameter.
    //
    // capture is *by value* - the place is read once, here, in the frame the closure is created in - so a
    // closure is always safe to outlive that frame. the returned read is of the environment's copy, which
    // is why a later write to the original is not observed
    //
    // null when the capture is refused, with the reason already reported
    AST::ExprNode *capture_variable(
        Payload &payload, AST::VarDeclNode *vardecl, const TokenReference &at, size_t boundaries_crossed);

    // parses a function's body, from its opening brace through its closing one, into `decl->body`.
    // `scope` is the parameter frame the body is pushed under, so a parameter resolves through it
    //
    // shared by parse_funcdecl and parse_closure_literal, which differ in exactly one thing: a closure
    // hands itself in, so a read of an enclosing local in the body is a capture rather than an error.
    // everything else - the function-boundary marker, the return-type and body frames, the recovery on a
    // missing brace - is one body of code, and it had to be edited in two places as long as it was two
    //
    // answers false when a brace is missing, having reported it and recovered
    bool parse_function_body(
        Payload &payload,
        AST::FunctionDeclNode &decl,
        AST::ScopeNode &scope,
        AST::ClosureExprNode *closure = nullptr);

    // reads a parameter list up to and *including* its closing token, appending each parameter to
    // `decl` and declaring it in `into`. the cursor must already be past the opening one
    //
    // shared with the struct parser's `constructor(...)`, which is an ordinary declaration in every
    // respect the signature is concerned with - the two used to carry a copy each, and the copies had
    // already drifted in how they recovered
    //
    // `closing` is what encloses the list, and the only caller that does not want a parenthesis is an
    // index operator's `[usize $i]` - a parameter list in every other respect, so it is this function
    // with one token changed rather than a second walk that would drift the way the two copies did
    //
    // answers false when the list runs off the end of the file, having reported that at `report_at`
    // and recovered: there is no partial parameter list worth carrying on with, because the arity is
    // what a call resolves against
    bool parse_parameter_list(
        Payload &payload,
        AST::FunctionDeclNode &decl,
        AST::ScopeNode &into,
        const TokenReference &report_at,
        Token::Type closing = Token::Type::t_close_paren);

    // prepends an implicit parameter - one the caller never writes - to `decl`, named `name` and typed
    // `type_node`, and declares it in `into` so it resolves exactly as any other parameter does
    //
    // a *parameter* rather than something codegen conjures, which is what makes a method and a closure
    // ordinary functions: mangling, cloning, the pointer adjuster and codegen all handle them with no
    // special case. the two kinds - a receiver and an environment - sit in the same slot and are counted
    // back out by the same `implicit_arg_count()`, so they are pushed by the same code
    void push_implicit_param(
        Payload &payload,
        AST::FunctionDeclNode &decl,
        AST::ScopeNode &into,
        const std::string &name,
        AST::TypeNode *type_node,
        const TokenReference &at);

    // **the modifiers written ahead of `function`.** one bundle read by one scan, so the next member
    // modifier - A17's `public`/`private` - is a field added here rather than a second parameter
    // threaded through parse_funcdecl and everything it hands the declaration to
    struct MemberModifiers
    {
        // where the `const` was written, so a refusal points at the modifier rather than at the name.
        // present *is* the modifier - a separate bool beside it would be a second carrier of one fact,
        // and the site that words the refusal reads the token through a `.value()` the bool does not
        // guard
        std::optional<TokenReference> const_token;

        // `const function get() : int32` - the method only reads, so its `$this` is `const Foo&`.
        // it goes no further than picking that TypeNode: from there the receiver's *type* is the
        // whole of the feature (AST::receiver_is_const), and nothing downstream carries a flag
        bool is_const() const { return const_token.has_value(); }
    };

    // the implicit `$this` receiver, typed `self_type` - the non-nullable borrow `Foo&` (or `Foo<T>&`),
    // or `const Foo&` for a method declared `const`. which one is Context::receiver_type's answer, and
    // this takes the node rather than the flag so there is exactly one place that picks
    //
    // shared by the method and destructor arms so the two receivers cannot drift - they are the same
    // thing, and a destructor that borrowed differently would mutate a copy and free nothing
    inline void push_receiver_param(
        Payload &payload,
        AST::FunctionDeclNode &decl,
        AST::ScopeNode &into,
        AST::TypeNode *self_type,
        const TokenReference &at)
    {
        push_implicit_param(payload, decl, into, "$this", self_type, at);
    }
};


#endif