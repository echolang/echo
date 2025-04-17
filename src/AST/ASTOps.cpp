#include "AST/ASTOps.h"

#include <stack>
#include <iostream>
#include <memory>


// the tiers are spaced by ten, and the spacing is the point rather than decoration: a custom
// operator declares its precedence as a number on *this* scale (`operator(45, left)`), so there has
// to be room between two built-in levels to declare into. the numbers themselves are published in
// book/concept/operators.md, which makes them a contract rather than an implementation detail
//
// **a smaller sequence binds tighter**, which is the one thing about this table that is easy to
// read backwards. AST::CUSTOM_OP_DEFAULT_PRECEDENCE sits at 95, between `|` and comparison
//
// write each arm's number out. deriving it from the old one would also move `default:`, and a
// non-zero default makes is_expr_token treat every token in the language as an expression token
AST::OpPrecedence AST::Operator::get_precedence_for_token(const Token::Type &type)
{
    switch (type) {
        // parantheses
        case Token::Type::t_open_paren:
        case Token::Type::t_close_paren:
            return {OpAssociativity::none, 10};

        // increment/decrement
        case Token::Type::t_op_inc:
        case Token::Type::t_op_dec:
            return {OpAssociativity::right, 20};

        // exponent
        case Token::Type::t_op_pow:
            return {OpAssociativity::right, 30};

        // multiplication, division, modulo
        case Token::Type::t_op_mul:
        case Token::Type::t_op_div:
        case Token::Type::t_op_mod:
            return {OpAssociativity::left, 40};

        // addition, subtraction
        case Token::Type::t_op_add:
        case Token::Type::t_op_sub:
            return {OpAssociativity::left, 50};

        // bitwise shift
        case Token::Type::t_op_shl:
        case Token::Type::t_op_shr:
            return {OpAssociativity::left, 60};

        // and, xor, or
        case Token::Type::t_and:
            return {OpAssociativity::left, 70};
        case Token::Type::t_xor:
            return {OpAssociativity::left, 80};
        case Token::Type::t_or:
            return {OpAssociativity::left, 90};

        // comparison
        case Token::Type::t_open_angle:
        case Token::Type::t_close_angle:
        case Token::Type::t_logical_geq:
        case Token::Type::t_logical_leq:
        case Token::Type::t_logical_eq:
        case Token::Type::t_logical_neq:
            return {OpAssociativity::left, 100};

        case Token::Type::t_logical_and:
            return {OpAssociativity::left, 110};

        case Token::Type::t_logical_or:
            return {OpAssociativity::left, 120};

        // assignment
        case Token::Type::t_assign:
            return {OpAssociativity::right, 130};

        // zero means "not an operator", which is what is_expr_token reads to decide whether a
        // token can appear in an expression at all. every real tier above is therefore non-zero
        default:
            return {OpAssociativity::none, 0};
    };
}

const char *AST::op_fixity_name(AST::OpFixity fixity)
{
    switch (fixity) {
        case OpFixity::t_infix: return "infix";
        case OpFixity::t_prefix: return "prefix";
        case OpFixity::t_suffix: return "suffix";
        case OpFixity::t_index: return "index";
    }

    return "unknown";
}

AST::OperatorRegistry::OperatorRegistry()
{
    _predefined_operator_map.fill(nullptr);

    // register the predefined operators
    register_predefined_token_op(Token::Type::t_assign);
    register_predefined_token_op(Token::Type::t_and);
    register_predefined_token_op(Token::Type::t_or);
    register_predefined_token_op(Token::Type::t_xor);
    register_predefined_token_op(Token::Type::t_open_paren);
    register_predefined_token_op(Token::Type::t_close_paren);
    register_predefined_token_op(Token::Type::t_logical_or);
    register_predefined_token_op(Token::Type::t_logical_and);
    register_predefined_token_op(Token::Type::t_logical_eq);
    register_predefined_token_op(Token::Type::t_logical_neq);
    register_predefined_token_op(Token::Type::t_open_angle);
    register_predefined_token_op(Token::Type::t_close_angle);
    register_predefined_token_op(Token::Type::t_logical_geq);
    register_predefined_token_op(Token::Type::t_logical_leq);
    register_predefined_token_op(Token::Type::t_op_shl);
    register_predefined_token_op(Token::Type::t_op_shr);
    register_predefined_token_op(Token::Type::t_op_add);
    register_predefined_token_op(Token::Type::t_op_sub);
    register_predefined_token_op(Token::Type::t_op_mul);
    register_predefined_token_op(Token::Type::t_op_div);
    register_predefined_token_op(Token::Type::t_op_mod);
    register_predefined_token_op(Token::Type::t_op_pow);
    register_predefined_token_op(Token::Type::t_op_inc);
    register_predefined_token_op(Token::Type::t_op_dec);
}

void AST::OperatorRegistry::register_predefined_token_op(const Token::Type &type)
{
    auto t = Token(type, 0, 0);
    assert(t.is_operator_type() && "Token type is not an operator"); // sanity check

    auto op = std::make_unique<PredefinedTokenOperator>(type);
    _predefined_operator_map[static_cast<size_t>(type)] = op.get();
    _operator_symbol_map[token_lit_symbol_string(type)] = op.get(); // also store it as a custom operator for easy lookup
    _operators.push_back(std::move(op));
}

AST::Operator *AST::OperatorRegistry::find_known_symbol(const std::string &spelling) const
{
    // an already known spelling answers with what is already there, whichever kind it is. for a
    // single token that is a predefined operator this is the *overloading* case - `operator (Point
    // $a) + (Point $b)` declares an overload of `+`, it does not mint a second `+` - and it has to
    // be this way round: `_operator_symbol_map` is one map for both kinds, and
    // `build_incdec_value` looks `"+"` up in it by string to desugar `$i++`, so a custom `+` here
    // would retype every increment in the program to t_op_custom and land it on codegen's throw
    if (auto existing = _operator_symbol_map.find(spelling); existing != _operator_symbol_map.end()) {
        return existing->second;
    }

    return nullptr;
}

AST::CustomOperator *AST::OperatorRegistry::mint_symbol(const std::string &spelling)
{
    auto op = std::make_unique<CustomOperator>(spelling);
    CustomOperator *published = op.get();

    _operator_symbol_map[spelling] = published;
    _operators.push_back(std::move(op));

    return published;
}

AST::Operator *AST::OperatorRegistry::find_or_declare(const std::vector<std::string> &symbol_tokens)
{
    if (symbol_tokens.empty()) {
        return nullptr;
    }

    std::string spelling;
    for (const auto &token_value : symbol_tokens) {
        spelling += token_value;
    }

    if (Operator *existing = find_known_symbol(spelling)) {
        return existing;
    }

    CustomOperator *published = mint_symbol(spelling);

    // one path per symbol, one node per token of it. the spelling lookup above is what guarantees a
    // path is only ever laid down once, so nothing here has to reconcile with an existing symbol
    SymbolTrieNode *node = &_symbol_trie;
    for (const auto &token_value : symbol_tokens) {
        auto &child = node->children[token_value];

        if (child == nullptr) {
            child = std::make_unique<SymbolTrieNode>();
        }

        node = child.get();
    }

    node->symbol = published;

    return published;
}

const char *AST::OperatorRegistry::bracket_spelling()
{
    return "[]";
}

AST::Operator *AST::OperatorRegistry::find_or_declare_bracket()
{
    const std::string spelling = bracket_spelling();

    if (Operator *existing = find_known_symbol(spelling)) {
        return existing;
    }

    // the spelling map and nothing else - see the header for why the trie must not learn this symbol
    return mint_symbol(spelling);
}

const AST::Operator *AST::OperatorRegistry::get_operator(const TokenReference &token) const
{
    if (!token.is_valid()) {
        return nullptr;
    }

    // one token, one predefined operator. a *custom* symbol is not reachable from a single token
    // at all - it may span several of them and its tokens carry their own ordinary types - so
    // match_at is the only thing that finds one
    if (token.token().is_operator_type()) {
        return _predefined_operator_map[static_cast<size_t>(token.type())];
    }

    return nullptr;
}

bool AST::OperatorRegistry::tokens_are_adjacent(
    const TokenReference &first, const TokenReference &second)
{
    return first.line() == second.line()
        && second.char_offset() == first.char_offset() + first.value().length();
}

AST::OperatorRegistry::Match AST::OperatorRegistry::match_custom_at(
    const TokenReference &start, size_t available) const
{
    const TokenCollection &collection = start.get_collection_ref();
    const size_t handle = start.get_handle();

    Match best;
    const SymbolTrieNode *node = &_symbol_trie;

    // `available` bounds the descent, so a symbol can never be matched out of the tokens of the file
    // that follows this one in the module's collection
    for (size_t i = 0; i < available; i++) {
        const TokenReference token = collection[handle + i];

        // the tokens of one symbol have to be written together, so `!!` is a symbol and `! !` is two
        // of something else - the same predicate the declaration reads to decide where a symbol ends
        if (i > 0 && !tokens_are_adjacent(collection[handle + i - 1], token)) {
            break;
        }

        const auto child = node->children.find(token.value());

        if (child == node->children.end()) {
            break;
        }

        node = child->second.get();

        // keep descending past a symbol that ends here: a longer one wins, so `+++` is not read as
        // `++` then `+`, and a declared `<=>` is not read as the predefined `<=` then `>`
        if (node->symbol != nullptr) {
            best = Match{node->symbol, i + 1};
        }
    }

    return best;
}

AST::OperatorRegistry::Match AST::OperatorRegistry::match_at(
    const TokenReference &start, size_t available) const
{
    if (!start.is_valid() || available == 0) {
        return {};
    }

    // asked before anything reads the token's value, because a program that declares no operator at
    // all is the common case and this keeps it from even hashing one
    if (!_symbol_trie.children.empty()) {
        // a custom symbol first: a declared symbol wins over the predefined tokens it is spelled out
        // of, which is what keeps `<=>` from being read as `<=` followed by `>`
        if (const Match custom = match_custom_at(start, available); custom.has()) {
            return custom;
        }
    }

    if (const Operator *predefined = get_operator(start)) {
        return Match{predefined, 1};
    }

    return {};
}

const AST::Operator *AST::OperatorRegistry::get_operator(const std::string &symbol) const
{
    auto custom_op = _operator_symbol_map.find(symbol);
    if (custom_op != _operator_symbol_map.end()) {
        return custom_op->second;
    }

    return nullptr;
}