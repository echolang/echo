#include "Parser/OperatorDeclParser.h"

#include "AST/ASTCollector.h"
#include "AST/ASTIssue.h"
#include "AST/ASTNamespace.h"
#include "AST/ASTOperatorSemantics.h"
#include "AST/ASTValueType.h"
#include "AST/TypeNode.h"
#include "Parser/FuncDeclParser.h"
#include "Parser/SymbolParser.h"
#include "Parser/TypeParser.h"

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
            case Token::Type::t_open_bracket:
            case Token::Type::t_close_bracket:
            case Token::Type::t_ref:
            case Token::Type::t_namespace_sep:
                return true;
            default:
                // every arithmetic, bitwise and comparison token. the parentheses are in there too,
                // but is_structural_token has already stopped the run on those
                return Token(type, 0, 0).is_operator_type();
        }
    }

    // are two tokens written with nothing between them? what makes `!!` one symbol and `! !` two of
    // something else. asked of the source positions rather than by counting, because a token's value
    // is its own spelling
    bool tokens_are_adjacent(const TokenReference &first, const TokenReference &second)
    {
        return first.line() == second.line()
            && second.char_offset() == first.char_offset() + first.value().length();
    }

    // skips a balanced `( ... )` group from its opening parenthesis. the header reader steps over a
    // parameter list it is not parsing - the type-name pass wants the symbol and nothing else
    void skip_paren_group(Parser::Cursor &cursor)
    {
        if (!cursor.is_type(Token::Type::t_open_paren)) {
            return;
        }

        int depth = 0;

        while (!cursor.is_done()) {
            if (cursor.is_type(Token::Type::t_open_paren)) {
                depth++;
            } else if (cursor.is_type(Token::Type::t_close_paren)) {
                depth--;
                if (depth == 0) {
                    cursor.skip();
                    return;
                }
            }

            cursor.skip();
        }
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

            cursor.skip();
        }

        Parser::skip_declaration_body(payload);
    }
}

void Parser::skip_operatordecl(Parser::Payload &payload)
{
    auto &cursor = payload.cursor;

    if (!starts_operatordecl(cursor)) {
        return;
    }

    cursor.skip(); // the `operator` keyword
    skip_operator_remainder(payload);
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
        skip_paren_group(cursor);
    }

    // the symbol: a maximal run of adjacent, non-structural tokens. both halves matter - the
    // structural stop is what keeps a `(` written tight against the symbol out of it, and the
    // adjacency is what makes `operator (int $a) not eq (int $b)` a located error about `not`
    // rather than a symbol read as `not` and a parse that then falls apart
    std::optional<TokenReference> previous;

    while (!cursor.is_done() && !is_structural_token(cursor.current().type())) {
        const TokenReference token = cursor.current();

        // adjacency is against the token just consumed, so a three token symbol checks each
        // neighbour rather than everything against the first
        if (previous.has_value() && !tokens_are_adjacent(*previous, token)) {
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

    AST::Operator *op = payload.collector.operators.find_or_declare(header.symbol_tokens);

    if (op == nullptr) {
        return;
    }

    // a declared symbol may sit in more than one position - `-` is infix and prefix - but a symbol
    // that is both infix and suffix is undecidable at `$a + $b`, where the yard cannot tell whether
    // the symbol closes the left operand or opens the right one
    const bool infix_suffix_clash =
        (header.fixity == AST::OpFixity::t_infix && op->has_fixity(AST::OpFixity::t_suffix))
        || (header.fixity == AST::OpFixity::t_suffix && op->has_fixity(AST::OpFixity::t_infix));

    if (infix_suffix_clash && header.symbol_token.has_value()) {
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
        // a *built-in* symbol's precedence is the language's, not a declaration's: `+` binds the way
        // it binds whatever anyone overloads it for, or two files would parse the same expression
        // differently
        if (!op->is_custom() && header.symbol_token.has_value()) {
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
            && (op->precedence.sequence != declared.sequence || op->precedence.assoc != declared.assoc)
            && header.symbol_token.has_value()) {
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

    // the header walks *over* the left operand list on its way to the symbol, because the fixity - and
    // therefore whether there is a left operand list at all - is not known until the symbol has been
    // read. so the position is kept and returned to, rather than the header parsing half a signature
    const auto at_keyword = cursor.snapshot();

    const OperatorHeader header = read_operator_header(payload);

    if (!header.valid) {
        skip_operator_remainder(payload);
        return nullptr;
    }

    const auto after_symbol = cursor.snapshot();

    // an operator is declared once, at file scope, and its symbol is global. so the two places it
    // could otherwise be written are refused here rather than half-supported:
    //
    //  - inside a `struct`, where it would read as a member of a type it is not a member of. the
    //    struct member walk refuses it before reaching this function, so this is the second gate
    //  - inside a `{ }` block, where every other declaration is block-scoped while this one's symbol
    //    would still be visible to the whole program - the shunting yard has one precedence table
    if (payload.context.self_struct_ptr != nullptr) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(operator_token),
            "An operator cannot be declared inside a struct. Declare it at file scope - an operator "
            "is not a member of either of its operand types.");
        skip_operator_remainder(payload);
        return nullptr;
    }

    if (payload.context.current_namespace != nullptr && payload.context.current_namespace->is_lexical()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(operator_token),
            "An operator cannot be declared inside a block. Declare it at file scope - its symbol is "
            "visible to the whole program, so it cannot be scoped to one.");
        skip_operator_remainder(payload);
        return nullptr;
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

    auto &funcscope = payload.context.emplace_node<AST::ScopeNode>();

    // the operand lists, in the order the fixity says they are written. an infix declaration has two
    // groups around the symbol, so the left one has to be parsed from a position the header already
    // walked past - the cursor is restored to it rather than the header returning half a parse
    if (header.fixity != AST::OpFixity::t_prefix) {
        cursor.restore(at_keyword);
        cursor.skip(); // the `operator` keyword

        // ...past the precedence clause, if there was one
        if (cursor.is_type(Token::Type::t_open_paren) && cursor.peek_is_type(1, Token::Type::t_integer_literal)) {
            skip_paren_group(cursor);
        }

        if (!cursor.is_type(Token::Type::t_open_paren)) {
            payload.collect_unexpected_token(Token::Type::t_open_paren);
            skip_declaration_body(payload);
            return nullptr;
        }

        cursor.skip(); // `(`

        if (!parse_parameter_list(payload, *funcdecl, funcscope, operator_token)) {
            return nullptr;
        }

        cursor.restore(after_symbol);
    }

    if (header.fixity != AST::OpFixity::t_suffix) {
        if (!cursor.is_type(Token::Type::t_open_paren)) {
            payload.collect_unexpected_token(Token::Type::t_open_paren);
            skip_declaration_body(payload);
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
    if (funcdecl->get_return_type().is_void()) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(*header.symbol_token),
            fmt::format(
                "operator '{}' returns void. An operator is an expression, so it has to return "
                "something.",
                header.spelling));
        skip_operator_remainder(payload);
        return nullptr;
    }

    // the arity the fixity promises. checked here rather than trusted, because a wrong count would
    // otherwise register a declaration no use site can ever reach: `match_function` compares arity
    // first, so it would simply never match and the operator would silently do nothing
    const size_t wanted_arity = header.fixity == AST::OpFixity::t_infix ? 2 : 1;

    if (funcdecl->args.size() != wanted_arity) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(*header.symbol_token),
            fmt::format(
                "an {} operator takes {} operand{}, but '{}' declares {}.",
                AST::op_fixity_name(header.fixity),
                wanted_arity,
                wanted_arity == 1 ? "" : "s",
                header.spelling,
                funcdecl->args.size()));
        skip_operator_remainder(payload);
        return nullptr;
    }

    // **two spellings no use site can reach**, this one and the suffix `++` below. both are refused
    // ahead of the all-primitive check further down, because "this spelling is not reachable" is the
    // more specific thing to say and it would otherwise be reported as the vaguer one
    //
    // `=` first: assignment is a statement, not an expression the shunting yard ever sees, so an
    // overload of it would register, mangle, be emitted, and never fire
    if (header.spelling == "=") {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(*header.symbol_token),
            "'=' cannot be declared as an operator - assignment is a statement, not an expression.");
        skip_operator_remainder(payload);
        return nullptr;
    }

    // **a suffix `++` / `--` cannot be reached.** `$i++;` is a statement, dispatched straight to
    // Parser::parse_varexpr and desugared there into `$i = $i + 1` so that every arithmetic rule
    // keeps one implementation. so a declaration of it would be silently ignored at the only
    // spelling of it that parses - said out loud rather than left as a puzzle
    if (header.fixity == AST::OpFixity::t_suffix
        && (header.spelling == "++" || header.spelling == "--")) {
        payload.collector.collect_issue<AST::Issue::GenericError>(
            payload.context.code_ref(*header.symbol_token),
            fmt::format(
                "'{}' cannot be declared as an operator: `$i{}` is a statement, and it always means "
                "`$i = $i {} 1`.",
                header.spelling, header.spelling, header.spelling.substr(0, 1)));
        skip_operator_remainder(payload);
        return nullptr;
    }

    // **a built-in symbol needs at least one declared operand.** codegen lowers every primitive and
    // pointer combination itself, and the built-in meaning wins - so an all-primitive overload of `+`
    // would register, mangle and be emitted, and then never fire. that is the class of silent no-op
    // publish_implicit_conversion refuses seven shapes for
    //
    // a *custom* symbol is exempt, and has to be: `operator (int32 $a)mm : Distance` is the point of
    // the whole feature, and its operand is a primitive
    const AST::Operator *op = payload.collector.operators.get_operator(header.spelling);

    if (op != nullptr && !op->is_custom()) {
        bool has_declared_operand = false;

        for (const auto *arg : funcdecl->args) {
            const AST::ValueType type = arg->has_type() ? arg->type() : AST::ValueType::make_unknown();
            has_declared_operand |= AST::value_type_of(type).has_complex_type() || type.is_type_param();
        }

        if (!has_declared_operand) {
            payload.collector.collect_issue<AST::Issue::GenericError>(
                payload.context.code_ref(*header.symbol_token),
                fmt::format(
                    "operator '{}' is built in for these operand types, so this declaration would "
                    "never be used. An overload of a built-in operator needs at least one declared "
                    "type among its operands.",
                    header.spelling));
            skip_declaration_body(payload);
            return nullptr;
        }
    }

    // **no type-parameter refusal, and deliberately none.** the grammar gives an operator no name for
    // a `<T>` to follow, so there is no way to introduce a type parameter and nothing to refuse - a
    // check for one would be dead code. an operator over a *concrete instantiation*,
    // `operator (Vec<int32> $a) + (Vec<int32> $b)`, is an ordinary declaration and works today. the
    // missing spelling is a grammar question, and it is todo/A32's

    // the signature is complete, so this is the earliest point it can join its overload set.
    // registering in both passes is intentional: the declaration pass makes it visible to use sites
    // written above it and in other files, and the body pass finds the same declaration site
    payload.collector.functions.register_function(
        payload.collector, payload.context.code_ref(*header.symbol_token), funcdecl);

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
