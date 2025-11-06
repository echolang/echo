#include "Parser/SymbolParser.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/NamespaceParser.h"
#include "Parser/TypeDeclParser.h"
#include "Parser/TypeParser.h"
#include "Parser/ExternParser.h"
#include "Parser/ScopeParser.h"
#include "Parser/AttributeParser.h"
#include "Parser/OperatorDeclParser.h"
#include "Parser/ConstDeclParser.h"

#include "AST/ASTSymbol.h"
#include "AST/TypeDeclNode.h"

#include <cassert>

void Parser::parse_type_names(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    // one entry per open `{`, innermost last, saying what that brace opened. a nested type is
    // registered on its *owner* rather than in the namespace and a name written in a *block* is
    // registered by neither, so this pass has to know which kind of region it is in - and it walks token
    // by token without ever consuming a body, which is what makes that a tracked stack rather than a
    // recursion. the stack's depth *is* the brace depth: two notions that had to agree are one
    struct OpenRegion
    {
        // the type whose body this brace opened, null for a function, method, constructor or bare block
        AST::TypeDeclNode *type_body;
    };

    std::vector<OpenRegion> open_regions;

    // the declaration just read, waiting for the `{` that opens its body. anything other than that
    // brace arriving next clears it: a malformed declaration must not adopt a later block
    AST::TypeDeclNode *pending_body = nullptr;

    while (!cursor.is_done()) {
        // `namespace a::b;` is a statement, not a block - it names the namespace the rest of the
        // file declares into, so this pass has to follow it to push a symbol into the right one
        //
        // **only at file scope**, because that is the only place the statement is legal:
        // parse_namespacedecl refuses one written inside a block, and its refusal reads
        // `current_namespace->is_lexical()` - never true in this pass, which opens no lexical scope. so
        // following it here would move every *later* declaration in the file into a namespace the two
        // passes that come after keep at the file's, and the refusal stays theirs to report
        if (cursor.is_type(Token::Type::t_namespace)) {
            if (open_regions.empty()) {
                parse_namespacedecl(payload);
                continue;
            }

            pending_body = nullptr;
            cursor.skip();
            continue;
        }

        // **an operator's symbol is published here, a whole pass earlier than any other declaration
        // publishes anything.** the expression parser has to know whether a symbol is an operator
        // before it parses a single use site, and the *declaration* pass already parses expressions
        // itself - a struct property's `= ...` initializer - so publishing there would make a
        // property initializer's operators depend on which file happened to be walked first
        //
        // only the symbol, its fixity and its precedence: no types, no namespace, no signature. that
        // is what makes this reachable from a pass whose whole contract is that it validates nothing
        // about declarations, and it is the deleted lexer prepass's job done at the right layer
        if (starts_operatordecl(cursor)) {
            const OperatorHeader header = read_operator_header(payload);

            // **only a declaration at file scope publishes.** an operator written inside a `struct`
            // body or inside a block is refused by parse_operatordecl, and a refusal has to decline to
            // *publish* to be worth anything: the symbol it would leave behind changes how every
            // expression in the program parses, so a declaration the compiler rejects would still
            // have rewritten the grammar the rest of the diagnostics are computed against
            //
            // one test for both, because any brace open here is one or the other - a namespace is a
            // statement in this language, not a block. reported by parse_operatordecl in the passes
            // that follow, which is what keeps this pass one that validates nothing
            if (open_regions.empty()) {
                publish_operator_symbol(payload, header);
            }

            // the rest of the declaration - its operand lists, return type and body - is walked token
            // by token by the loop below, which is what keeps `brace_depth` right
            pending_body = nullptr;
            continue;
        }

        if (!starts_typedecl(cursor) || !cursor.peek_is_type(1, Token::Type::t_identifier)) {
            // token by token rather than skipping bodies whole, which is also how parse_symbols
            // walks - so a `struct` written inside a function body is reached by both
            if (cursor.is_type(Token::Type::t_open_brace)) {
                // every brace pushes a region, carrying the declaration that opened it or nothing.
                // pushing only the type bodies is what made this two counters that had to agree
                open_regions.push_back(OpenRegion { pending_body });
            }
            else if (cursor.is_type(Token::Type::t_close_brace)) {
                if (!open_regions.empty()) {
                    open_regions.pop_back();
                }
            }

            pending_body = nullptr;
            cursor.skip();
            continue;
        }

        // **a type written in a block is registered by neither of the two arms below.** its name
        // belongs to the block's *lexical* namespace, and only the two later passes can mint that: the
        // namespace is named after the enclosing function, which this pass does not parse a signature to
        // learn, and AST::retrieve_lexical is create-or-reuse - so a namespace minted here with no name
        // to give it is the object those passes would then reuse, stripping the `outer::` prefix off
        // every block-local diagnostic in the program. parse_typedecl publishes it in pass 2 instead,
        // where the scope exists; a body-local name is visible in one block of one file, and this pass
        // is here so a name written *out of order across files* resolves
        if (!open_regions.empty() && open_regions.back().type_body == nullptr) {
            pending_body = nullptr;
            cursor.skip(); // the struct or class keyword, and the loop walks the rest of it
            continue;
        }

        // the struct whose body this declaration sits directly inside, if any. an empty stack is the
        // file's own namespace, and every other region was answered above
        AST::TypeDeclNode *owner = open_regions.empty() ? nullptr : open_regions.back().type_body;

        const AST::ComplexTypeKind kind = typedecl_kind(cursor);

        cursor.skip(); // the struct or class keyword

        auto name_token = cursor.current();
        cursor.skip();

        // this pass opens no lexical scope, and the one statement that could have moved it elsewhere is
        // declined above - so the namespace here is always one the user wrote, which is what makes the
        // three uses of it below the file's own
        assert(!payload.context.current_namespace->is_lexical());

        // **find before mint**, a strict subset of Parser::publish_type_symbol's question and asked one
        // step earlier because this pass would otherwise mint a second node for the name and declare its
        // type parameters. the duplicate is left symbol-less deliberately and reported nowhere here:
        // both later passes then find the *first* node and parse_typedecl reports the redeclaration at
        // the duplicate's own name token, which is also where the body-skip recovery lives
        //
        // a nested type is in no namespace, so the same question is asked of its owner instead
        if (owner != nullptr) {
            if (owner->complex_type().find_member_type_decl(name_token.value()) != nullptr) {
                continue;
            }
        }
        else {
            auto *existing =
                payload.collector.namespaces.find_symbol(name_token.value(), *payload.context.current_namespace);
            if (existing != nullptr && existing->node.get_ptr<AST::TypeDeclNode>() != nullptr) {
                continue;
            }
        }

        // the kind is settled here, at the only place the node is created. parse_typedecl reuses this
        // node in both later passes, so it never has to re-derive it from the keyword
        auto &type_node = payload.context.emplace_node<AST::TypeDeclNode>(name_token, kind);
        type_node.set_namespace(payload.context.current_namespace);

        // the type parameters, not only the name: for a generic type the arity is part of its
        // identity, and parse_generic_application reads the arity off the template to check an
        // application against it. without this `struct Holder { Box<int32> $b; }` written *above*
        // `struct Box<T>` reports "wrong number of type arguments" against a template that has not
        // had its list read yet - the declaration pass reaches Holder's property first. one level of
        // dependency per pass is the rule, and an arity is a name's, not a signature's
        //
        // declare_type_parameters is idempotent by reuse, so the two later passes reaching the same
        // list keep *these* declarations rather than minting their own - which they have to, or
        // Box<T> would intern twice and the two would compare unequal
        Parser::declare_type_parameters(payload, type_node.complex_type(), parse_type_param_list(payload));

        // a nested type goes on the owner and *not* into the namespace, so a bare `view` never
        // resolves at file scope. parse_typedecl reaches the same node in both later passes by asking
        // the owner, exactly as it reaches a top-level one by asking the namespace
        if (owner != nullptr) {
            owner->complex_type().add_member_type(name_token.value(), &type_node);
        }
        else {
            Parser::publish_type_symbol(payload, *payload.context.current_namespace, type_node);
        }

        // the body this declaration is about to open, so the `{` arriving next knows whose it is
        pending_body = &type_node;
    }
}

void Parser::parse_symbols(Parser::Payload &payload)
{
    // this pass has no file root - the body pass builds that - but it does parse declarations, and
    // parse_varexpr takes the scope a *declaration* goes into as an argument while writing any
    // *statement* it reads into the ambient `context.scope()`, which would assert with nothing
    // pushed. a scratch scope so that path has somewhere to land; it guards parse_varexpr's ambient
    // write rather than expressing an intent to collect anything
    auto &declaration_scope = payload.context.emplace_node<AST::ScopeNode>();
    payload.context.push_scope(declaration_scope);

    parse_declaration_surface(payload);

    payload.context.pop_scope();
}

void Parser::parse_declaration_surface(Parser::Payload &payload, std::optional<TokenReference> block_token)
{
    auto &cursor = payload.cursor;

    // a block's declarations belong to the block. minted here as well as in parse_scope, on the same
    // brace, so both passes reach one namespace object: this pass is where a declaration actually joins
    // its overload set, and the body pass's calls have to look it up in the same place
    AST::LexicalScope lexical_scope(payload.context, payload.collector.namespaces, block_token);

    if (block_token.has_value()) {
        cursor.skip(); // the opening brace
    }

    while (!cursor.is_done()) {
        // functions do not become namespace symbols: a symbol slot holds one node per name, and a
        // name denotes an overload *set*. parse_funcdecl registers them in
        // Collector::functions instead, which is also why `struct Foo` and its constructor `Foo`
        // no longer fight over the same slot
        if (starts_funcdecl(cursor)) {
            parse_funcdecl(payload);
        }
        else if (starts_operatordecl(cursor)) {
            // the *signature*. the symbol itself was published a pass ago, in parse_type_names, so
            // this pass is free to name a type from any file in its operand list - and a use site
            // written above this declaration already knows the symbol is an operator
            parse_operatordecl(payload);
        }
        else if (starts_typedecl(cursor)) {
            // the name is already a symbol - parse_type_names pushed it, over every file, before
            // this pass started. that is what lets the declarations below name a type from any file
            parse_typedecl(payload);
        }
        else if (cursor.is_type(Token::Type::t_extern)) {
            // walked in this pass too, so the node it produces already knows it is extern. otherwise
            // a cross-module call would resolve to this node and mangle the Echo name while codegen
            // emitted the raw C symbol - an undefined symbol at link time. registration happens
            // inside parse_funcdecl, same as any other function
            parse_extern_block(payload);
        }
        else if (cursor.is_type(Token::Type::t_hash)) {
            // an attribute, walked in this pass too - the frames of this walk must mirror the body
            // pass's exactly, and until now this one skipped `#` as an unknown token. that made every
            // attribute a *body-pass* fact, which is fine for `#[builtin: ...]` (read off the same
            // declaration node either pass reconciles on) but not for anything the next pass has to
            // already know: `#[core: "string"]` binds the type a string literal is, and a literal is
            // parsed in the body pass. binding it here is what makes that independent of file order,
            // since this pass completes over *every* file before the next one starts
            parse_attribute(payload);
        }
        else if (cursor.is_type(Token::Type::t_namespace)) {
            parse_namespacedecl(payload);
        }
        else if (starts_constdecl(payload)) {
            // a compile-time constant, name *and* initializer, in this pass - the same standing a struct
            // property's initializer has, and for the same reason: this pass completes over every file
            // before any body is parsed, so a constant is nameable from anywhere in the program regardless
            // of which file declared it. References inside the initializer are resolved later still, by
            // AST::ConstantExpander, so not even two constants have an order between them.
            //
            // **and this is the one owner of the body refusal.** `block_token.has_value()` is exactly
            // "inside a function body or a block", which is where a constant may not be: it has no storage
            // to be local to, and every use site gets a copy of its expression. The body pass skips the
            // statement silently on the strength of this arm having spoken
            if (block_token.has_value()) {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(cursor.current()),
                    "A constant cannot be declared inside a body - its value is copied to every use site, "
                    "so it belongs at file, namespace or struct scope. Write `const $name = ...;` for a "
                    "block-local variable that is const.");
                cursor.try_skip_to_next_statement();
            }
            else {
                parse_constdecl(payload, nullptr);
            }
        }
        else if (cursor.is_type(Token::Type::t_open_brace)) {
            // a bare nested block, which is its own declaration scope - the body pass mirrors this in
            // parse_scope, keyed on the same brace
            parse_declaration_surface(payload, cursor.current());
        }
        else if (block_token.has_value() && cursor.is_type(Token::Type::t_close_brace)) {
            cursor.skip(); // the region's own closing brace
            return;
        }
        else {
            cursor.skip();
        }
    }
}