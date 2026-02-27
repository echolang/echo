#include "Parser/OperatorDeclParser.h"

#include "AST/ASTCollector.h"
#include "AST/ASTIssue.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTValueType.h"
#include "AST/TypeDeclNode.h"
#include "AST/TypeNode.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/SymbolParser.h"
#include "Parser/TypeParser.h"

#include <algorithm>
#include <fmt/core.h>

namespace
{
    // the tokens a symbol may not be spelled out of, and the tokens that therefore *end* one. this is
    // the token-level form of the character predicate the deleted lexer prepass used, and it is
    // load-bearing in both directions: without it `operator (Point $a)+(Point $b)` written without
    // spaces reads the `(` as part of the symbol, because it is adjacent to the `+`
    bool is_structural_token(Token::Type type)
    {
        switch (type) {
            case Token::Type::t_open_paren:
            case Token::Type::t_close_paren:
            case Token::Type::t_open_brace:
            case Token::Type::t_close_brace:
            case Token::Type::t_comma:
            case Token::Type::t_semicolon:
            case Token::Type::t_colon:
                return true;
            default:
                return false;
        }
    }

    // which tokens a symbol may be spelled out of: an identifier, which is how a word operator like
    // `avg` or `mm` arrives, or one of the punctuation tokens the lexer already has
    //
    // **`[` and `]` are not on it**, and their absence is load-bearing rather than an omission. a
    // bracket belongs to exactly one production - the index form read above this list's caller - and
    // that exclusivity is what lets a use site's `[` be claimed unconditionally by the postfix chain,
    // which is in turn what makes `$p:$[0]` the only spelling of pointer indexing. a symbol
    // allowed to contain one would be a second contract on the same character
    //
    // a **keyword** is refused, and that is the whole of what this list is for: matching happens on
    // token *values*, so a symbol spelled `if` would turn every `if` in the program into an operator.
    // spelled as an allow-list rather than "everything that is not a keyword", so a keyword added
    // later is refused by default instead of quietly becoming declarable
    bool is_allowed_symbol_token(Token::Type type)
    {
        switch (type) {
            case Token::Type::t_identifier:
            case Token::Type::t_exclamation:
            case Token::Type::t_qmark:
            case Token::Type::t_dot:
            case Token::Type::t_accessorlr:
            case Token::Type::t_ptr_of:
            case Token::Type::t_hash:
            case Token::Type::t_ref:
            case Token::Type::t_namespace_sep:
                return true;
            default:
                // every arithmetic, bitwise and comparison token. the parentheses are in there too,
                // but is_structural_token has already stopped the run on those
                return Token(type, 0, 0).is_operator_type();
        }
    }

    // steps over a parameter list this reader is not parsing - the type-name pass wants the symbol and
    // nothing else, and the two later passes come back to a recorded position. the depth walk itself
    // is Parser::Cursor::skip_balanced_group, which owns the skip vocabulary
    void skip_paren_group(Parser::Cursor &cursor)
    {
        cursor.skip_balanced_group(Token::Type::t_open_paren, Token::Type::t_close_paren);
    }

    // the same, for the index form's `[ ... ]`
    void skip_bracket_group(Parser::Cursor &cursor)
    {
        cursor.skip_balanced_group(Token::Type::t_open_bracket, Token::Type::t_close_bracket);
    }
}

bool Parser::starts_operatordecl(Parser::Cursor &cursor)
{
    return cursor.is_type(Token::Type::t_operator);
}

namespace
{
    // consumes the rest of a declaration from wherever inside it the cursor happens to be, and then
    // its body. every refusal in this file goes through it, because Parser::skip_declaration_body
    // expects the cursor to be *on* the body - and a declaration refused at its symbol is not
    //
    // parenthesised groups are stepped over whole, so an operand list cannot be mistaken for the end
    void skip_operator_remainder(Parser::Payload &payload)
    {
        auto &cursor = payload.cursor;

        while (!cursor.is_done()
            && !cursor.is_type(Token::Type::t_open_brace)
            && !cursor.is_type(Token::Type::t_semicolon)) {

            if (cursor.is_type(Token::Type::t_open_paren)) {
                skip_paren_group(cursor);
                continue;
            }

            // an index operator's operand list, stepped over whole for the same reason the
            // parenthesised ones are: what is inside an operand list is not where this declaration
            // ends, whichever bracket encloses it
            if (cursor.is_type(Token::Type::t_open_bracket)) {
                skip_bracket_group(cursor);
                continue;
            }

            cursor.skip();
        }

        Parser::skip_declaration_body(payload);
    }
}

Parser::OperatorHeader Parser::read_operator_header(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;
    OperatorHeader header;

    if (!starts_operatordecl(cursor)) {
        return header;
    }

    const TokenReference operator_token = cursor.current();
    cursor.skip();

    // the type-parameter list, `operator<T>`. it sits where a function writes one - immediately after
    // the thing that names the declaration - and the `operator` keyword is that thing, since an
    // operator has no name token of its own
    //
    // told from a **prefix `<` operator** by looking two tokens past the angle: a type parameter is an
    // identifier followed by `,`, `>` or a `:` constraint, while `operator <(int32 $a) : bool` has a
    // `(` there. the same shape as the precedence clause's lookahead below, and the same reason - one
    // token of context is what separates two productions that start alike
    if (cursor.is_type(Token::Type::t_open_angle)
        && cursor.peek_is_type(1, Token::Type::t_identifier)
        && (cursor.peek_is_type(2, Token::Type::t_comma)
            || cursor.peek_is_type(2, Token::Type::t_close_angle)
            || cursor.peek_is_type(2, Token::Type::t_colon))) {

        // parsed rather than skipped, so the list's grammar has one owner - and **kept**, so it is
        // parsed once: minting the declarations from it is parse_operatordecl's step, and in the two
        // later passes this walk resolves every constraint atom, which is not work to do twice
        header.type_params = Parser::parse_type_param_list(payload);
    }

    // the precedence clause, `operator(45, left)`. told from a parameter group by the one token after
    // the parenthesis: a parameter list opens with a type, and no type production in the language
    // starts with an integer literal
    if (cursor.is_type(Token::Type::t_open_paren) && cursor.peek_is_type(1, Token::Type::t_integer_literal)) {
        cursor.skip(); // `(`

        header.precedence = std::stoi(cursor.current().value());
        cursor.skip();

        if (cursor.is_type(Token::Type::t_comma)) {
            cursor.skip();

            if (cursor.is_type(Token::Type::t_identifier) && cursor.current().value() == "left") {
                header.associativity = AST::OpAssociativity::left;
                cursor.skip();
            } else if (cursor.is_type(Token::Type::t_identifier) && cursor.current().value() == "right") {
                header.associativity = AST::OpAssociativity::right;
                cursor.skip();
            } else {
                payload.collector.collect_issue<AST::Issue::GenericError>(
                    payload.context.code_ref(cursor.is_done() ? operator_token : cursor.current()),
                    "An operator's associativity is written 'left' or 'right'.");
                return header;
            }
        }

        if (!cursor.is_type(Token::Type::t_close_paren)) {
            payload.collect_unexpected_token(Token::Type::t_close_paren);
            return header;
        }

        cursor.skip(); // `)`
    }

    // a leading parameter group means the symbol comes after its left operand: infix or suffix.
    // anything else means the symbol comes first, which is prefix
    const bool has_left_operand = cursor.is_type(Token::Type::t_open_paren);

    if (has_left_operand) {
        // where the list starts, for parse_operatordecl to come back to
        header.left_params = cursor.snapshot();
        skip_paren_group(cursor);
    }

    // **the index form**, `operator (array<T>& $a)[usize $i]`. it is recognised here, ahead of the
    // symbol run, and never *by* the symbol run: its two tokens are not adjacent, so there is no run
    // to read. the bracket also cannot appear in any other symbol - is_allowed_symbol_token refuses
    // both of them - so a `[` in this position can only mean one thing and needs no lookahead
    //
    // the spelling is synthesised rather than concatenated from what was written, because what was
    // written has an operand list in the middle of it
    if (has_left_operand && cursor.is_type(Token::Type::t_open_bracket)) {
        header.symbol_token.emplace(cursor.current());
        header.spelling = AST::OperatorRegistry::bracket_spelling();
        header.symbol_tokens.push_back(header.spelling);
        header.fixity = AST::OpFixity::t_index;
        header.index_params = cursor.snapshot();

        skip_bracket_group(cursor);

        // **the write form**, `operator (map<K, V>& $m)[const K& $key] = (V $value) : void`. one token of
        // lookahead tells it from the borrowing form, and one is enough: after the bracket group the only
        // thing the borrowing form can have is its `:`, and an `=` cannot be part of the symbol because
        // the symbol was synthesised as `[]` rather than read from the tokens
        //
        // the spelling stays `[]` and the symbol token stays the `[`. the *fixity* is what differs, which
        // is what mints the second decorated name and therefore the second overload set - the position a
        // bracket sits in is then the whole of what chooses between the two contracts
        if (cursor.is_type(Token::Type::t_assign)) {
            header.fixity = AST::OpFixity::t_index_write;
            cursor.skip(); // `=`

            if (!cursor.is_type(Token::Type::t_open_paren)) {
                payload.collect_unexpected_token(Token::Type::t_open_paren);
                return header;
            }

            header.value_params = cursor.snapshot();
            skip_paren_group(cursor);
        }

        header.valid = true;
        return header;
    }

    // the symbol: a maximal run of adjacent, non-structural tokens. both halves matter - the
    // structural stop is what keeps a `(` written tight against the symbol out of it, and the
    // adjacency is what makes `operator (int $a) not eq (int $b)` a located error about `not`
    // rather than a symbol read as `not` and a parse that then falls apart
    std::optional<TokenReference> previous;

    while (!cursor.is_done() && !is_structural_token(cursor.current().type())) {
        const TokenReference token = cursor.current();

        // adjacency is against the token just consumed, so a three token symbol checks each
        // neighbour rather than everything against the first. the registry's predicate, because a use
        // site is matched with the same one - see AST::OperatorRegistry::tokens_are_adjacent
        if (previous.has_value()
            && !AST::OperatorRegistry::tokens_are_adjacent(*previous, token)) {
            break;
        }

        if (!is_allowed_symbol_token(token.type())) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(token),
                fmt::format(
                    "'{}' cannot be part of an operator symbol - a symbol is written out of "
                    "punctuation or a single word.",
                    token.value()));
            return header;
        }

        // `>>` is split token by token when it closes a generic argument list, which the cursor
        // tracks as a half-consumed token. a symbol *containing* one would be read against that
        // state, so it is refused rather than left to disagree. overloading `>>` itself is fine -
        // that is one token and the predefined operator, not a new symbol
        if (token.type() == Token::Type::t_op_shr && !header.symbol_tokens.empty()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(token),
                "'>>' cannot appear inside a longer operator symbol - it is also how a nested "
                "generic argument list closes.");
            return header;
        }

        // emplaced rather than assigned: TokenReference holds a collection reference, so the
        // optional has no copy-assignment - the same reason FunctionDeclNode's name token is set at
        // construction and never afterwards
        if (!header.symbol_token.has_value()) {
            header.symbol_token.emplace(token);
        }

        header.symbol_tokens.push_back(token.value());
        header.spelling += token.value();
        previous.emplace(token);

        cursor.skip();
    }

    if (header.symbol_tokens.empty()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(operator_token),
            "An operator declaration needs a symbol, e.g. 'operator (Point $a) + (Point $b) : Point'.");
        return header;
    }

    // the fixity falls out of where the parameter groups were: a group before *and* after the symbol
    // is infix, only before is suffix, only after is prefix
    if (has_left_operand) {
        header.fixity = cursor.is_type(Token::Type::t_open_paren)
            ? AST::OpFixity::t_infix
            : AST::OpFixity::t_suffix;
    } else {
        header.fixity = AST::OpFixity::t_prefix;
    }

    header.valid = true;
    return header;
}

void Parser::publish_operator_symbol(Parser::Payload &payload, const OperatorHeader &header)
{
    if (!header.valid) {
        return;
    }

    // the bracket is minted through its own path, which registers the spelling and stops there. see
    // OperatorRegistry::find_or_declare_bracket: a trie entry for `[` `]` would match the append form
    // `$a[]` in the shunting yard, where the postfix chain has already claimed the token
    //
    // **one registry entry for both bracket forms**, carrying two fixity bits. a second entry keyed
    // `[]=` would buy nothing - `[` is never matched from the trie in the first place - and the two
    // *overload sets* are what keep the contracts apart, which is the decorated name's job rather than
    // the registry's
    AST::Operator *op = AST::is_index_fixity(header.fixity)
        ? payload.collector.operators.find_or_declare_bracket()
        : payload.collector.operators.find_or_declare(header.symbol_tokens);

    if (op == nullptr) {
        return;
    }

    // a declared symbol may sit in more than one position - `-` is infix and prefix - but a symbol
    // that is both infix and suffix is undecidable at `$a + $b`, where the yard cannot tell whether
    // the symbol closes the left operand or opens the right one
    const bool infix_suffix_clash =
        (header.fixity == AST::OpFixity::t_infix && op->has_fixity(AST::OpFixity::t_suffix))
        || (header.fixity == AST::OpFixity::t_suffix && op->has_fixity(AST::OpFixity::t_infix));

    if (infix_suffix_clash) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(*header.symbol_token),
            fmt::format(
                "'{}' is already declared as an {} operator. One symbol cannot be both infix and "
                "suffix - '$a {} $b' would not say which.",
                header.spelling,
                op->has_fixity(AST::OpFixity::t_infix) ? "infix" : "suffix",
                header.spelling));
        return;
    }

    if (header.precedence.has_value()) {
        // **an index operator has no precedence to declare.** `[` is consumed by the postfix chain,
        // which binds tighter than every binary operator by construction and never reaches the
        // shunting yard - so a number here would be stored, compared against nothing and silently do
        // nothing. refused where it is written, like every other unreachable spelling in this file
        if (AST::is_index_fixity(header.fixity)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(*header.symbol_token),
                "An index operator cannot declare a precedence - '[' binds like '->', tighter than "
                "every binary operator, and never reaches the precedence table.");
            return;
        }

        // a *built-in* symbol's precedence is the language's, not a declaration's: `+` binds the way
        // it binds whatever anyone overloads it for, or two files would parse the same expression
        // differently
        if (!op->is_custom()) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(*header.symbol_token),
                fmt::format(
                    "'{}' already has a precedence, so an overload of it cannot declare one.",
                    header.spelling));
            return;
        }

        const AST::OpPrecedence declared{
            header.associativity.value_or(AST::OpAssociativity::left),
            *header.precedence};

        // one symbol, one precedence. a second declaration writing a *different* clause is a
        // conflict; one writing no clause at all is not, so the check is against
        // `precedence_declared` rather than against the value the default left behind
        if (op->precedence_declared
            && (op->precedence.sequence != declared.sequence || op->precedence.assoc != declared.assoc)) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(*header.symbol_token),
                fmt::format(
                    "'{}' is already declared with precedence {}, so it cannot also be declared with "
                    "precedence {}. A symbol binds one way everywhere.",
                    header.spelling, op->precedence.sequence, declared.sequence));
            return;
        }

        op->precedence = declared;
        op->precedence_declared = true;
    }

    op->declare_fixity(header.fixity);
}

AST::FunctionDeclNode *Parser::parse_operatordecl(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    // the declaration pass stops once the signature is registered; the body pass carries on into the
    // body. read off the payload for parse_funcdecl's reason - no caller in between has to know
    const bool symbol_only = payload.pass == Pass::t_declarations;

    const TokenReference operator_token = cursor.current();

    const OperatorHeader header = read_operator_header(payload);

    // every refusal below is "report it here, then consume the declaration whole", which is what lets
    // the struct member walk hand one to this function instead of refusing it itself. one lambda
    // rather than the three statements written out per refusal, publish_implicit_conversion's shape -
    // nine copies of a recovery is nine chances for one of them to recover differently
    const auto refuse = [&](const TokenReference &at, const std::string &message)
        -> AST::FunctionDeclNode * {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(at), message);
        skip_operator_remainder(payload);
        return nullptr;
    };

    if (!header.valid) {
        skip_operator_remainder(payload);
        return nullptr;
    }

    const auto after_symbol = cursor.snapshot();

    // an operator is declared once, at file scope, and its symbol is global. so the two places it
    // could otherwise be written are refused here rather than half-supported:
    //
    //  - inside a `struct`, where it would read as a member of a type it is not a member of. the
    //    struct member walk routes one here rather than reporting it there, so this is the only
    //    spelling of that diagnostic
    //  - inside a `{ }` block, where every other declaration is block-scoped while this one's symbol
    //    would still be visible to the whole program - the shunting yard has one precedence table
    // **an interface body is the one exception, and it declares a requirement rather than an operator.**
    // the refusal above is about a *definition*: an operator is not a member of its operand types, so a
    // struct body has nothing to own. an interface owns no operator either - the implementor still
    // declares it at file scope, in the one global set - but it may *require* one, and a requirement is
    // exactly the kind of thing an interface body holds. so this is not a fourth registration path, it
    // is the method-requirement path with an operator's signature
    AST::TypeDeclNode *interface_owner = nullptr;

    if (payload.context.self_struct_ptr != nullptr) {
        if (payload.context.self_struct_ptr->complex_type().is_interface_kind()) {
            interface_owner = payload.context.self_struct_ptr;
        }
        else {
            return refuse(operator_token,
                "An operator cannot be declared inside a struct. Declare it at file scope - an operator "
                "is not a member of either of its operand types.");
        }
    }

    if (payload.context.current_namespace != nullptr && payload.context.current_namespace->is_lexical()) {
        return refuse(operator_token,
            "An operator cannot be declared inside a block. Declare it at file scope - its symbol is "
            "visible to the whole program, so it cannot be scoped to one.");
    }

    const std::string decorated_name = AST::operator_function_name(header.spelling, header.fixity);

    // a module's passes must land on *one* node per declaration. the `operator` keyword is what they
    // reconcile on: it is a real token at a fixed index and unique per declaration, exactly the role
    // a constructor's `constructor` keyword plays - and unlike the name below, which is minted
    AST::FunctionDeclNode *funcdecl = payload.collector.functions.find_by_declaration_site(operator_token);

    if (funcdecl != nullptr) {
        // the arguments are rebuilt against this pass's context, so drop the previous pass's
        funcdecl->args.clear();
    } else {
        // the name is **virtual**: an operator has no name token, and the decorated spelling is what
        // FunctionRegistry keys the overload set on. positioned at the symbol so every diagnostic
        // about this declaration points where a reader would look
        //
        // minted only here, when the node is created. TokenReference has no copy-assignment, and
        // re-minting on the body pass would let the two passes disagree about func_name() - which is
        // the map key, so the declaration would end up in the overload set twice under two names
        const TokenReference name_token = payload.context.make_virtual_token(
            decorated_name, Token::Type::t_identifier, *header.symbol_token);

        funcdecl = &payload.context.emplace_node<AST::FunctionDeclNode>(name_token, operator_token);
    }

    funcdecl->member_kind = AST::MemberKind::t_operator;

    // the **root** namespace, always. `Namespace::overloads()` stops at the first namespace with a
    // candidate - outer sets are hidden, not extended - which is right for a name and wrong for `+`,
    // where one symbol names the set for every type in the program: an operator in an inner namespace
    // would hide every unrelated one. the precedence table is already global, so the set is too
    funcdecl->ast_namespace = &payload.collector.namespaces.root();

    // the type parameters, declared and made visible before a single operand type is read - a
    // parameter mentioned in `(array<T>& $a)` has to resolve while that list is parsed.
    // parse_funcdecl's order exactly, and the same two calls: declare_type_parameters owns the shape
    // of FunctionDeclNode::type_parameters, TypeParamScope owns their visibility
    //
    // no inherited parameters are passed: an operator is never a member, so there is no owner whose
    // list would sit ahead of its own. the list itself was parsed by read_operator_header, which had
    // to walk it to reach the symbol
    declare_type_parameters(payload, *funcdecl, header.type_params);

    AST::TypeParamScope type_param_scope(payload.context, funcdecl->type_parameters);

    auto &funcscope = payload.context.emplace_node<AST::ScopeNode>();

    // the operand lists, in the order the fixity says they are written. an infix declaration has two
    // groups around the symbol, so the left one has to be parsed from a position the header already
    // walked past - the cursor is restored to the position the header recorded rather than that
    // position being derived a second time
    if (header.fixity != AST::OpFixity::t_prefix) {
        cursor.restore(*header.left_params);
        cursor.skip(); // `(`

        if (!parse_parameter_list(payload, *funcdecl, funcscope, operator_token)) {
            return nullptr;
        }

        cursor.restore(after_symbol);
    }

    // the index form's second operand list sits *inside* the symbol, so it is parsed from its own
    // recorded position rather than from wherever the symbol ended. an empty `[]` is the append form
    // and parses to no parameters at all, which is the whole of how the two are told apart later:
    // one overload set, separated by arity, which is what match_function compares first
    if (AST::is_index_fixity(header.fixity)) {
        cursor.restore(*header.index_params);
        cursor.skip(); // `[`

        if (!parse_parameter_list(payload, *funcdecl, funcscope, operator_token,
                Token::Type::t_close_bracket)) {
            return nullptr;
        }

        cursor.restore(after_symbol);
    }

    // **the write form's value, parsed third**, which is what makes `args` read
    // `[receiver, indices..., value]` - the order a use site writes them in and the order
    // AST::OperatorRewriter builds the call's operands in. the three restores are what decide it, so
    // moving one moves the operand a body reads
    if (header.fixity == AST::OpFixity::t_index_write) {
        cursor.restore(*header.value_params);
        cursor.skip(); // `(`

        if (!parse_parameter_list(payload, *funcdecl, funcscope, operator_token)) {
            return nullptr;
        }

        cursor.restore(after_symbol);
    }

    if (header.fixity == AST::OpFixity::t_prefix || header.fixity == AST::OpFixity::t_infix) {
        if (!cursor.is_type(Token::Type::t_open_paren)) {
            payload.collect_unexpected_token(Token::Type::t_open_paren);
            skip_operator_remainder(payload);
            return nullptr;
        }

        cursor.skip(); // `(`

        if (!parse_parameter_list(payload, *funcdecl, funcscope, operator_token)) {
            return nullptr;
        }
    }

    if (!cursor.is_type(Token::Type::t_colon)) {
        payload.collect_unexpected_token(Token::Type::t_colon);
        skip_operator_remainder(payload);
        return nullptr;
    }

    cursor.skip(); // `:`

    if (!can_parse_type(payload)) {
        payload.collect_unexpected_token(Token::Type::t_identifier);
        skip_operator_remainder(payload);
        return nullptr;
    }

    funcdecl->return_type = parse_type(payload);

    // after the return type, for parse_funcdecl's reason: the return type is the last thing an
    // attribute could have something to say about, and leaving them staged lets the next `struct`
    // drain them
    Parser::drain_attributes(payload, funcdecl->attributes);

    // an operator is an expression, so a void one is a statement written as an operator. refused
    // rather than lowered, because `$a = $b avg $c` would then declare a void variable
    //
    // **the index write is the exception, and its rule is the mirror rather than an exemption.** that
    // form *is* a statement - `$m[$k] = $v` produces nothing, AST::OperatorRewriter replaces the whole
    // assignment with the call - so void is not merely tolerated there but required. two refusals and
    // not one escape, because the void-ness is the content of the form
    if (header.fixity == AST::OpFixity::t_index_write) {
        if (!funcdecl->get_return_type().is_void()) {
            return refuse(*header.symbol_token,
                fmt::format(
                    "an index-write operator is a statement, not an expression - '$c[$k] = $v' produces "
                    "no value, so it returns 'void' and not '{}'. Declare the borrowing form "
                    "'operator (C& $c)[K $k] : V&' if you want a place a write goes through.",
                    funcdecl->get_return_type().get_type_desciption()));
        }
    }
    else if (funcdecl->get_return_type().is_void()) {
        return refuse(*header.symbol_token,
            fmt::format(
                "operator '{}' returns void. An operator is an expression, so it has to return "
                "something.",
                header.spelling));
    }

    // **the write has to reach the caller's container.** by value it lands in a copy that dies with the
    // call, and through a `const` borrow it does not land at all - both register, both are chosen, and
    // both silently do nothing, which is what every other refusal in this file exists to prevent.
    //
    // the borrowing form needs no such rule: its whole product is the borrow it returns, which the
    // return-type refusal below already judges - a `const C&` receiver there yields a `const V&`
    // element and the ordinary const rules take it from there
    if (header.fixity == AST::OpFixity::t_index_write && !funcdecl->args.empty()) {
        const AST::ValueType receiver = funcdecl->parameter_type(0);

        if (!receiver.is_pointer() || receiver.is_nullable() || receiver.pointee().is_const()) {
            return refuse(*header.symbol_token,
                fmt::format(
                    "an index-write operator takes its container as a mutable borrow - 'C&', not '{}'. "
                    "The write has to reach the caller's container: by value it would land in a copy "
                    "that dies with the call, and through a 'const' borrow it would not land at all.",
                    receiver.get_type_desciption()));
        }
    }

    // **an index operator hands back the element itself**, and `$a[$i]` is a place unconditionally -
    // it reads, writes, takes `&` and chains with `->` through the one address path, exactly as
    // `$p:$[$i]` does. an overload returning by value would make place-ness depend on which overload
    // won, which is the split AST::is_place_expression exists to prevent, so the contract is the
    // return type: a non-nullable borrow, nothing else
    if (header.fixity == AST::OpFixity::t_index
        && !(funcdecl->get_return_type().is_pointer() && !funcdecl->get_return_type().is_nullable())) {
        return refuse(*header.symbol_token,
            fmt::format(
                "an index operator returns a borrow of the element - 'T&', not '{}'. `$a[$i]` is a "
                "place, so what it names has to have an address.",
                funcdecl->get_return_type().get_type_desciption()));
    }

    // the arity the fixity promises. checked here rather than trusted, because a wrong count would
    // otherwise register a declaration no use site can ever reach: `match_function` compares arity
    // first, so it would simply never match and the operator would silently do nothing
    //
    // the index forms are the ones with a *range* rather than a count. for the borrowing form one
    // operand is the append slot, `&$a[]`; two or more is an element, `$a[$i]` and `$m[$row, $col]`.
    // they share one overload set and one decorated name, and arity is what tells them apart - so the
    // rule here is only that the receiver is present, and match_function does the rest with no new rule
    //
    // the write form's range starts one higher, because the value it is given is not optional: two
    // operands is the append write `$c[] = $v`, three or more an element write
    if (AST::is_index_fixity(header.fixity)) {
        const size_t minimum = header.fixity == AST::OpFixity::t_index_write ? 2 : 1;

        if (funcdecl->args.size() < minimum) {
            return refuse(*header.symbol_token,
                minimum == 1
                    ? std::string(
                        "an index operator takes the container as its first operand, e.g. "
                        "'operator (array<int32>& $a)[usize $i] : int32&'.")
                    : std::string(
                        "an index-write operator takes the container, then its indices, then the value "
                        "it is given, e.g. "
                        "'operator (map<K, V>& $m)[const K& $key] = (V $value) : void'. Two operands is "
                        "the append write '$c[] = $v'."));
        }
    } else {
        const size_t wanted_arity = header.fixity == AST::OpFixity::t_infix ? 2 : 1;

        if (funcdecl->args.size() != wanted_arity) {
            return refuse(*header.symbol_token,
                fmt::format(
                    "an {} operator takes {} operand{}, but '{}' declares {}.",
                    AST::op_fixity_name(header.fixity),
                    wanted_arity,
                    wanted_arity == 1 ? "" : "s",
                    header.spelling,
                    funcdecl->args.size()));
        }
    }

    // **two spellings no use site can reach**, this one and the suffix `++` below. both are refused
    // ahead of the built-in-meaning check further down, because "this spelling is not reachable" is
    // the more specific thing to say and it would otherwise be reported as the vaguer one
    //
    // `=` first: assignment is a statement, not an expression the shunting yard ever sees, so an
    // overload of it would register, mangle, be emitted, and never fire
    //
    // the index write does not reach here - its spelling is `[]`, synthesised by the header - and the
    // message names it because it is the one assignment that *is* declarable: not `=` over two operands,
    // but the bracket a container declares a write contract for
    if (header.spelling == "=") {
        return refuse(*header.symbol_token,
            "'=' cannot be declared as an operator - assignment is a statement, not an expression. The "
            "one assignment that is declarable is the index write, "
            "'operator (C& $c)[K $k] = (V $v) : void'.");
    }

    // **a suffix `++` / `--` cannot be reached.** `$i++;` is a statement, dispatched straight to
    // Parser::parse_varexpr and desugared there into `$i = $i + 1` so that every arithmetic rule
    // keeps one implementation. so a declaration of it would be silently ignored at the only
    // spelling of it that parses - said out loud rather than left as a puzzle
    if (header.fixity == AST::OpFixity::t_suffix
        && (header.spelling == "++" || header.spelling == "--")) {
        return refuse(*header.symbol_token,
            fmt::format(
                "'{}' cannot be declared as an operator: `$i{}` is a statement, and it always means "
                "`$i = $i {} 1`.",
                header.spelling, header.spelling, header.spelling.substr(0, 1)));
    }

    // **a declaration the built-in meaning would win over.** codegen lowers a whole matrix of operand
    // types itself, and where it does, a declaration would register, mangle and be emitted, and then
    // never fire. that is the class of silent no-op publish_implicit_conversion refuses seven shapes
    // for
    //
    // asked of **the** predicate, AST::binary_has_builtin_meaning, which is also what the parser reads
    // at a use site to decide whether to look for a declaration and what the type checker reads to
    // report that none was found. re-deriving it here as "does any operand have a complex type" was a
    // third answer to one question, and it differed: `==` over two class handles is an address
    // comparison codegen *does* lower, so such a declaration was accepted and could never fire
    //
    // a custom symbol answers false for every operand, which is what keeps
    // `operator (int32 $a)mm : Distance` - the point of the whole feature, over a primitive - declarable
    // both index forms are exempt, and not by omission: `[` has no built-in meaning over a *complex*
    // base at all - the language spells one only for a pointer, which the rewriter keeps for itself -
    // so there is nothing for a declaration to be shadowed by. the write form would also index
    // `operands[1]` on a two-operand append write, where the second operand is the value rather than
    // anything the predicate is about
    //
    // **not asked of a requirement.** both this and the bare-type-parameter refusal inside it are about a
    // declaration that would be *chosen* at a use site and then never fire. a requirement is chosen at no
    // use site at all - it names the shape an implementor's own file-scope operator has to have, and that
    // declaration is the one these refusals judge. `interface Comparable<T> { operator (T $a) < (T $b); }`
    // is precisely the shape the bare-parameter arm exists to reject in a definition, and precisely the
    // shape a requirement is for
    if (!AST::is_index_fixity(header.fixity) && interface_owner == nullptr) {
        const AST::Operator *op = payload.collector.operators.get_operator(header.spelling);

        // the declared operand types as the predicate wants them: value-position, which is what a
        // parameter written `Point&` means once its operand has been read through
        std::vector<AST::OperandFacts> operands;
        for (const auto *arg : funcdecl->args) {
            const AST::ValueType type = arg->has_type() ? arg->type() : AST::ValueType::make_unknown();
            operands.push_back(AST::OperandFacts{AST::value_type_of(type)});
        }

        const bool builtin_wins = header.fixity == AST::OpFixity::t_infix
            ? AST::binary_has_builtin_meaning(op, operands[0], operands[1])
            : AST::unary_has_builtin_meaning(op, operands[0]);

        if (builtin_wins) {
            // **a bare type parameter is the same refusal with a different reason.** the predicate
            // admits an undeterminable operand deliberately - it says nothing either way - so
            // `operator<T> (T $a) + (T $b)` lands here, and "built in for these operand types" is
            // not what a reader wrote. an operator over a type parameter would have to be chosen
            // per instantiation, and the symbol is one global set with no receiver to key on
            const bool over_bare_param = std::any_of(operands.begin(), operands.end(),
                [](const AST::OperandFacts &facts) { return facts.type.is_type_param(); });

            if (over_bare_param) {
                return refuse(*header.symbol_token,
                    fmt::format(
                        "operator '{}' cannot be declared over a bare type parameter - the language "
                        "already spells a meaning for '{}' over the primitives a parameter may be "
                        "bound to. Declare it for the type itself, e.g. 'operator (Vec<T> $a) {} "
                        "(Vec<T> $b)'.",
                        header.spelling, header.spelling, header.spelling));
            }

            return refuse(*header.symbol_token,
                fmt::format(
                    "operator '{}' is built in for these operand types, so this declaration would "
                    "never be used - where the language spells a meaning, the built-in one wins.",
                    header.spelling));
        }
    }

    // **a type parameter that no operand mentions can never be bound.** a call site has a spelling
    // for explicit type arguments and an operator use site does not - `$a[$i]` carries nothing but
    // its operands - so inference is the only way one is ever decided. left unrefused, the
    // declaration would register and every single use site would report UnresolvedTypeParameter,
    // which is the same class of silent-until-used failure the built-in check above prevents
    for (const auto *param : funcdecl->type_parameters) {
        const bool mentioned = std::any_of(funcdecl->args.begin(), funcdecl->args.end(),
            [param](const AST::VarDeclNode *arg) {
                return arg != nullptr && arg->has_type() && AST::contains_type_param(arg->type(), param);
            });

        if (!mentioned) {
            return refuse(*header.symbol_token,
                fmt::format(
                    "type parameter '{}' is not mentioned by any operand of operator '{}', so nothing "
                    "could ever bind it - an operator use site has no spelling for explicit type "
                    "arguments.",
                    param->name, header.spelling));
        }
    }

    // the signature is complete, so this is the earliest point it can join its overload set.
    // registering in both passes is intentional: the declaration pass makes it visible to use sites
    // written above it and in other files, and the body pass finds the same declaration site
    //
    // **a requirement joins its interface's member table instead of the global operator set.** it has no
    // body and no symbol, so a use site must never be able to choose it - `$a < $b` has to resolve to the
    // implementor's own declaration or to nothing. `ast_namespace` stays the root above, because that is
    // where AST::first_unmet_requirement looks the implementor's operator *up*
    if (interface_owner != nullptr) {
        funcdecl->owner_type = &interface_owner->complex_type();

        payload.collector.functions.register_member_function(
            payload.collector, payload.context.code_ref(*header.symbol_token), funcdecl,
            interface_owner->complex_type());
    }
    else {
        payload.collector.functions.register_function(
            payload.collector, payload.context.code_ref(*header.symbol_token), funcdecl);
    }

    // what the attributes drained above publish about this declaration, through the one list of it -
    // so a marker added there reaches this site too rather than being remembered at a fourth. a null
    // owner is what tells `#[implicit]` that this is a free declaration, which is the refusal an
    // operator owes: a conversion is inserted where the user wrote *nothing*, and every spelling an
    // operator has is operand syntax the user writes
    publish_declaration_markers(payload, funcdecl, nullptr, *header.symbol_token);

    // a requirement ends here, in every pass, and it is deliberately **not** added to the declaration
    // scope: codegen emits bodies from that list, and there is no body to emit. the same tail an
    // `extern` owns, and for the same reason - the shape of the declaration decides where it ends
    if (interface_owner != nullptr) {
        if (!cursor.is_type(Token::Type::t_semicolon)) {
            return refuse(*header.symbol_token,
                fmt::format(
                    "operator '{}' is a requirement of the interface '{}', so it cannot have a body - "
                    "end it with ';' and declare the operator itself at file scope.",
                    header.spelling, interface_owner->type_name()));
        }

        cursor.skip(); // the semicolon
        return funcdecl;
    }

    if (symbol_only) {
        if (cursor.is_type(Token::Type::t_open_brace)) {
            // exactly the frames the body pass opens below, for parse_funcdecl's reason: the two
            // passes reading one declaration differently means the first to reach it wins
            AST::FunctionBodyScope body_scope(payload.context, funcdecl);
            Parser::parse_declaration_surface(payload, cursor.current());
        }

        return funcdecl;
    }

    // emitted from the file root like every other declaration - codegen walks `file.root->children`
    // and OwnershipPass resolves drops from the same list
    payload.context.declaration_scope().add_funcdecl(*funcdecl);

    if (!parse_function_body(payload, *funcdecl, funcscope)) {
        return nullptr;
    }

    return funcdecl;
}
