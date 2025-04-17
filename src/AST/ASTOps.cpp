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

AST::Operator *AST::OperatorRegistry::find_or_declare(const std::vector<std::string> &symbol_tokens)
{
    if (symbol_tokens.empty()) {
        return nullptr;
    }

    std::string spelling;
    for (const auto &token_value : symbol_tokens) {
        spelling += token_value;
    }

    // an already known spelling answers with what is already there, whichever kind it is. for a
    // single token that is a predefined operator this is the *overloading* case - `operator (Point
    // $a) + (Point $b)` declares an overload of `+`, it does not mint a second `+` - and it has to
    // be this way round: `_operator_symbol_map` is one map for both kinds, and
    // `build_incdec_value` looks `"+"` up in it by string to desugar `$i++`, so a custom `+` here
    // would retype every increment in the program to t_op_custom and land it on codegen's throw
    if (auto existing = _operator_symbol_map.find(spelling); existing != _operator_symbol_map.end()) {
        return existing->second;
    }

    auto op = std::make_unique<CustomOperator>(symbol_tokens, spelling);
    Operator *published = op.get();

    _operator_symbol_map[spelling] = published;
    _custom_operators.push_back(op.get());
    _operators.push_back(std::move(op));

    return published;
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

AST::OperatorRegistry::Match AST::OperatorRegistry::match_at(
    const TokenReference &start, size_t available) const
{
    if (!start.is_valid() || available == 0) {
        return {};
    }

    Match best;

    // a custom symbol first, longest one wins: `+++` must not be read as `++` followed by `+`, and
    // a declared `<=>` must not be read as the predefined `<=` followed by `>`
    for (const CustomOperator *candidate : _custom_operators) {
        const size_t length = candidate->symbol_tokens.size();

        if (length > available || length <= best.token_count) {
            continue;
        }

        bool matches = true;

        for (size_t i = 0; i < length && matches; i++) {
            const TokenReference token = start.get_collection_ref()[start.get_handle() + i];

            if (token.value() != candidate->symbol_tokens[i]) {
                matches = false;
                break;
            }

            // the tokens of one symbol have to be written together, so `!!` is a symbol and `! !`
            // is two of something else. checked against the previous token's end rather than by
            // counting characters, because a token's value is its own spelling
            if (i > 0) {
                const TokenReference previous = start.get_collection_ref()[start.get_handle() + i - 1];
                const bool adjacent = token.line() == previous.line()
                    && token.char_offset() == previous.char_offset() + previous.value().length();

                matches = adjacent;
            }
        }

        if (matches) {
            best = Match{candidate, length};
        }
    }

    if (best.has()) {
        return best;
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