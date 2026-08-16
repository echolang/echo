#ifndef FUNCDECLPARSER_H
#define FUNCDECLPARSER_H

#pragma once

#include "AST/ScopeNode.h"
#include "AST/VarDeclNode.h"
#include "AST/ASTContext.h"
#include "AST/FunctionDeclNode.h"
#include "Parser/ParserPayload.h"
#include "Parser/VisibilityParser.h"

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
    // stack for whatever comes along next, which is how a method's `#[implicit]` would land on the
    // following `struct`. taken as an AttributeList rather than a FunctionDeclNode for exactly that
    // reason: the struct site is the other half of that bug and drains through here too. called once
    // the declaration node is in hand, which is also the earliest point at which an attribute has
    // something to be read against
    void drain_attributes(Payload &payload, AST::AttributeList &into);

    // publishes every marker a member declaration's attributes state about it, or reports why the
    // declaration cannot carry one.
    //
    // the one list of "what does an attribute publish about a member", so a second marker - whatever
    // default arguments will bring - is added here and reaches the method, constructor and destructor sites at
    // once instead of being remembered at three. the drain stays at each site, because a declaration
    // reads `#[intrinsic]`/`#[builtin]` off its own list before it gets here
    void publish_declaration_markers(
        Payload &payload,
        AST::FunctionDeclNode *funcdecl,
        AST::TypeDeclNode *owner_struct,
        const TokenReference &nametoken);

    // publishes `#[implicit]` on `owner_struct`, or reports why the declaration cannot carry it.
    //
    // two shapes, one slot: a parameterless method is outbound (this type becomes the return type);
    // a static of one by-value parameter is inbound (the parameter becomes this type). the one owner of the
    // marker: what it means, which shapes are refused, and how each refusal reads. every declaration
    // that can be written with one reaches it through publish_declaration_markers - a free function
    // and a constructor or destructor only so that they can be told they cannot be one, which is a
    // diagnostic no method-only helper could ever report
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

    // whether the body is parsed or left for the body pass is read off `payload.pass`.
    //
    // `visibility` is what the dispatch that reached this declaration already consumed - see
    // MemberModifiers on why that one modifier arrives from outside while `const` does not. **required**,
    // and deliberately: VisibilityPrefix's own default is `t_public`, which is the position's answer for a
    // member and the opposite of it for a top-level declaration - so a caller that omitted one would
    // silently export what its author scoped to a module
    AST::FunctionDeclNode *parse_funcdecl(
        Payload &payload,
        FuncDeclKind kind,
        VisibilityPrefix visibility
    );

    // consumes a declaration's body from its first token: either a braced body or the bare `;` of a
    // declaration that has none. the one place that knows how a declaration body is skipped, shared by
    // the pass that does not read member bodies and by the recovery for a refused `function`
    //
    // brace-depth aware rather than token-by-token, because a body's closing brace would otherwise read
    // as the end of the enclosing scope - silently truncating a struct and losing every member written
    // after it, or resuming *inside* a body full of semicolons and reporting a cascade of nonsense
    void skip_declaration_body(Payload &payload);

    // recovery for a `function` this parser has decided not to read: past its signature and its whole
    // body. **the recovery every declaration-level refusal owes**, because Cursor::try_skip_to_next_statement
    // stops at the first `;` or `}` and a body is full of both - so it resumes *inside* the body it meant
    // to skip, and leaves that body's closing brace to be reported against whatever comes next
    //
    // named here rather than kept private to FuncDeclParser.cpp because a refused closure is the same
    // shape and the same problem, and a second copy of the skip is a second answer to where a body ends
    void skip_refused_function(Payload &payload);

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
        Payload &payload,
        AST::VarDeclNode *vardecl,
        const TokenReference &at,
        size_t boundaries_crossed
    );

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

    // **does an access effect start here, and which one?** `read` / `inout` / `out` at the head of a
    // parameter, answered in two tokens.
    //
    // these are *contextual* - matched as identifiers, with no entry in the lexer - and that is the
    // whole of why they cost nothing. an Echo keyword is global and word-boundary checked, so a real
    // `t_read` would make `$file->read()` unspellable in every program ever written, to buy a
    // distinction only a parameter list has ever needed.
    //
    // the second token is what disambiguates: a parameter is `<type> $name`, so an identifier
    // followed by a `$name` is the *type* of that parameter and never an effect. `read $x` therefore
    // stays a parameter of type `read`, and `read slice<T> $src` is a read access over a slice
    //
    // `mv` is deliberately not here: it is a real keyword already, it is read one line away, and
    // giving it a second reader would be two answers to which token said `take`
    bool starts_access_effect(Parser::Cursor &cursor, AST::AccessEffect &effect);

    // reads a parameter list up to and *including* its closing token, appending each parameter to
    // `decl` and declaring it in `into`. the cursor must already be past the opening one
    //
    // shared with the struct parser's `constructor(...)`, which is an ordinary declaration in every
    // respect the signature is concerned with. a copy in each parser would drift in how they recover
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

    // **the modifiers written ahead of `function`.** one bundle, so a modifier is a field added here rather
    // than a second parameter threaded through parse_funcdecl and everything it hands the declaration to.
    //
    // the two are read at two moments, and that split is deliberate rather than an inconsistency. `const`
    // is read *here*, by parse_funcdecl's own scan, because it belongs to the `function` grammar - only a
    // function has a receiver to make const. Visibility is read by Parser::parse_visibility_prefix
    // *before* the dispatch that decided this was a function at all, because `public const int32 $x;`,
    // `public const MAX = 5;` and `public const function f()` are three different declarations and the four
    // predicates that tell them apart all scan from the head of the statement
    struct MemberModifiers
    {
        // who may name it, already narrowed to Visibility::t_owner if this is a member - the position that
        // decided it is a place this parser no longer knows it was in
        VisibilityPrefix visibility;

        // where the `const` was written, so a refusal points at the modifier rather than at the name.
        // present *is* the modifier - a separate bool beside it would be a second carrier of one fact,
        // and the site that words the refusal reads the token through a `.value()` the bool does not
        // guard
        std::optional<TokenReference> const_token;

        // where the `static` was written, on the same "present *is* the modifier" terms as `const`
        // above. the two are mutually exclusive and parse_funcdecl refuses the pair: `const` says what
        // the receiver may do and a static has none, so `const static function` asks a question about
        // a parameter that is not there
        std::optional<TokenReference> static_token;

        // `const function get() : int32` - the method only reads, so its `$this` is `const Foo&`.
        // it goes no further than picking that TypeNode: from there the receiver's *type* is the
        // whole of the feature (AST::receiver_is_const), and nothing downstream carries a flag
        bool is_const() const { return const_token.has_value(); }

        // `static function make(int32 $v) : Box` - the function is owned by the type and takes no
        // receiver. unlike `const` this one does not pick a TypeNode, it suppresses one: the whole of
        // what it does is skip push_receiver_param and set MemberKind::t_static_method
        bool is_static() const { return static_token.has_value(); }
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
        const TokenReference &at
    )
    {
        push_implicit_param(payload, decl, into, "$this", self_type, at);
    }
};


#endif
