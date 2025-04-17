#ifndef ASTOPS_H
#define ASTOPS_H

#pragma once

#include "Token.h"

#include <vector>
#include <unordered_map>
#include <memory>
#include <array>

namespace AST
{
    enum class OpAssociativity
    {
        left,
        right,
        none
    };

    // where an operator's symbol sits relative to its operands, which is what tells the expression
    // parser *which* of its three positions may consume the symbol. one declaration has exactly one
    // of these; a symbol may be declared in more than one, and `-` is both infix and prefix
    enum class OpFixity
    {
        t_infix,
        t_prefix,
        t_suffix,

        // `$a[$i]` - the symbol brackets its *second* operand rather than standing beside it, which
        // is why it is a fixity of its own rather than an infix `[`. it is also the one fixity no
        // expression position consumes by looking it up: `[` is claimed unconditionally by the
        // postfix chain, and which meaning it has is decided from the base's type afterwards
        t_index,
    };

    const char *op_fixity_name(OpFixity fixity);

    struct OpPrecedence
    {
        OpAssociativity assoc;

        // **smaller binds tighter.** the built-in tiers are spaced by ten in
        // Operator::get_precedence_for_token, and a custom operator declares a number on that same
        // scale - one scale, so `operator(45, left)` is comparable with `*` at 40 and `+` at 50
        //
        // zero is reserved for "not an operator"
        int sequence;
    };

    // what a custom operator gets when its declaration writes no `(N, assoc)` clause: looser than
    // every arithmetic and bitwise operator, tighter than every comparison. so `1 + 2 avg 3 + 4`
    // groups its operands first, `(1 + 2) avg (3 + 4)`, and `$a avg $b > 3` compares the answer
    constexpr int CUSTOM_OP_DEFAULT_PRECEDENCE = 95;

    struct Operator
    {
        static OpPrecedence get_precedence_for_token(const Token::Type &type);

        const Token::Type type;

        // the symbol as a reader wrote it - "+", "avg", "!!". held on the base so one accessor
        // renders any operator: a diagnostic and OperatorNode::node_description both want it, and a
        // multi-token symbol has no single token whose value would do
        const std::string spelling;

        // not const: a custom symbol's precedence is whatever its declaration says, and the
        // declaration is read after the operator has been minted. `precedence_declared` is what
        // makes a second, *different* clause a conflict while a defaulted one is not
        OpPrecedence precedence;
        bool precedence_declared = false;

        Operator(const Token::Type type, const std::string &spelling, const OpPrecedence precedence) :
            type(type),
            spelling(spelling),
            precedence(precedence)
        {
        }

        // a symbol the user declared, rather than one the language spells. no *token* ever carries
        // Token::Type::t_op_custom - the lexer knows nothing about custom operators, and a declared
        // symbol is matched as a token sequence by OperatorRegistry::match_at - so this type is
        // purely the discriminator that tells the two kinds of Operator apart
        inline bool is_custom() const {
            return type == Token::Type::t_op_custom;
        }

        // the operators that ask a question about two operands rather than combining them
        // the distinction matters for pointers: comparing an address against a non-address is
        // a type error, while `$p:$ + 1` mixing an address and an int is ordinary offsetting
        inline bool is_comparison() const {
            return is_identity_comparison()
                || type == Token::Type::t_open_angle
                || type == Token::Type::t_close_angle
                || type == Token::Type::t_logical_leq
                || type == Token::Type::t_logical_geq;
        }

        // the two comparisons that ask "is this the same thing", rather than ordering it. the only
        // operators a class handle answers: two handles compare as addresses, and a handle compares
        // against null. every ordering operator on a class stays a type error
        inline bool is_identity_comparison() const {
            return type == Token::Type::t_logical_eq
                || type == Token::Type::t_logical_neq;
        }

        // **which positions a user has declared this symbol in.** filled by the parser's first pass,
        // read by the three expression positions that may consume a symbol - so the shunting yard
        // does not try a suffix-only `mm` as an infix operator, and `is_expr_token` only lets a
        // symbol *begin* an expression when it is declared prefix
        inline void declare_fixity(OpFixity fixity) {
            _fixities |= (1u << static_cast<unsigned>(fixity));
        }

        inline bool has_fixity(OpFixity fixity) const {
            return (_fixities & (1u << static_cast<unsigned>(fixity))) != 0;
        }

        // is there any declared `operator` for this symbol at all? a use site asks `has_fixity`
        // instead - each of the three positions is a different question, and a fixity implies a
        // declaration - so this is the coarse form, for asking whether a symbol has been claimed
        //
        // both are read off the *operator table*, filled by the type-name pass, and deliberately not
        // off "does the overload set have candidates": the overload set is filled by the declaration
        // pass, which itself parses expressions (a struct property initializer), so asking it there
        // would answer differently depending on file order
        inline bool is_declared() const {
            return _fixities != 0;
        }

        virtual ~Operator() = default;

    private:
        unsigned _fixities = 0;
    };

    struct PredefinedTokenOperator : public Operator
    {
        PredefinedTokenOperator(const Token::Type &type) :
            Operator(type, token_lit_symbol_string(type), get_precedence_for_token(type))
        {
        }
    };

    struct CustomOperator : public Operator
    {
        CustomOperator(const std::string &spelling) :
            Operator(Token::Type::t_op_custom, spelling,
                OpPrecedence{OpAssociativity::left, CUSTOM_OP_DEFAULT_PRECEDENCE})
        {
        }
    };

    class OperatorRegistry
    {
    public:
        // an operator found at a position in the token stream, together with how many tokens it
        // spans - one for every predefined operator, and however many the declared symbol lexes as
        // for a custom one
        struct Match
        {
            const Operator *op = nullptr;
            size_t token_count = 0;

            inline bool has() const {
                return op != nullptr;
            }
        };

        OperatorRegistry();
        ~OperatorRegistry() = default;

        void register_predefined_token_op(const Token::Type &type);

        // the operator a declaration names, minted if this is the first declaration of it.
        // `symbol_tokens` are the token values of the declared symbol, in order
        //
        // a **single** token whose value already spells a predefined operator answers with that
        // predefined operator instead of minting a custom one, so `operator (Point $a) + (Point $b)`
        // is an overload of `+` rather than a second, shadowing `+`. that is not a convenience:
        // `_operator_symbol_map` is one map for both kinds and `build_incdec_value` looks `"+"` up
        // in it by string, so minting a custom `+` would retype every `$i++` in the program
        Operator *find_or_declare(const std::vector<std::string> &symbol_tokens);

        // the bracket operator, `operator (Array<T>& $a)[usize $i]`, minted on first declaration.
        //
        // **it is registered by spelling only, never into the symbol trie**, and that is the whole
        // reason it is not find_or_declare({"[", "]"}). the trie is keyed on *adjacent* token values,
        // and in the append form `$a[]` the two brackets genuinely are adjacent - so a trie entry
        // would make `$a[]` match as a custom symbol inside the shunting yard, in the one position
        // where the postfix chain has already claimed the token. nothing may ever match `[`:
        // parse_postfix_chain owns it unconditionally, and AST::OperatorRewriter decides what it means
        Operator *find_or_declare_bracket();

        // the spelling every bracket operator shares. one constant rather than a literal at the four
        // sites that build or compare the decorated name
        static const char *bracket_spelling();

        const Operator *get_operator(const TokenReference &token) const;
        const Operator *get_operator(const std::string &symbol) const;

        // the longest operator whose symbol starts at `start`, or {} when none does
        //
        // `available` is how many tokens remain in the region being parsed, and it is passed in
        // rather than derived from `start`: a module's TokenCollection holds every one of its files
        // back to back, so walking forward with TokenReference::next() would let a symbol at the end
        // of one file match into the next
        //
        // the tokens of a multi-token symbol must be **adjacent** in the source, so `! !` is not
        // `!!`. a longer symbol wins over a shorter one, so `+++` is not read as `++` then `+`
        Match match_at(const TokenReference &start, size_t available) const;

        // **are two tokens written with nothing between them?** the whole of what makes several
        // tokens one symbol, and it is one predicate because both sides of the feature ask it: the
        // declaration, deciding where the symbol ends, and match_at, deciding whether a use site
        // spells one. two copies could disagree, and what disagreement looks like is a symbol
        // declarable in a shape no use site ever matches
        //
        // asked of the source positions rather than by counting, because a token's value is its own
        // spelling
        static bool tokens_are_adjacent(const TokenReference &first, const TokenReference &second);

    private:
        // **a node of the declared-symbol trie, keyed on token values.** the root's children are every
        // token a declared symbol may start with, a path from the root spells one symbol's tokens in
        // order, and `symbol` is set on the node where a symbol ends
        //
        // this is the structure rather than a list because match_at is asked at every operand and
        // every operator position of every expression in the program: scanning the declared symbols
        // there made parsing cost (symbols x positions), and measured, 64 declared operators put 57%
        // on top of parsing a 670 KB file - a cost that grew with how much a program used the feature.
        // a descent costs the length of the longest symbol that could match, whatever the program
        // declared, and it is the same shape as the lexer's LexerFunction::TreeNode one level down
        //
        // it also *is* the longest-match rule rather than implementing it: descending past a match
        // and keeping the deepest one is what makes `+++` not `++` then `+`
        //
        // the children are keyed on whole token *values*, which is what keeps a word operator from
        // eating the head of an identifier - the trap the old prefix-matching lexer entry had, where
        // `mm` matched the front of `mmap`
        struct SymbolTrieNode
        {
            std::unordered_map<std::string, std::unique_ptr<SymbolTrieNode>> children;
            CustomOperator *symbol = nullptr;
        };

        // the deepest declared symbol spelled by the tokens at `start`, or {}. split out of match_at
        // so the predefined answer stays one comparison away from the top of it
        Match match_custom_at(const TokenReference &start, size_t available) const;

        // the two halves both declaring paths share: is this spelling already claimed, and mint one
        // that is not. **interning lives here and only here** - the two declaring entry points differ
        // in exactly one thing, whether the symbol also enters the trie, and everything else about how
        // an Operator is stored has one owner
        Operator *find_known_symbol(const std::string &spelling) const;
        CustomOperator *mint_symbol(const std::string &spelling);

        std::vector<std::unique_ptr<Operator>> _operators;
        SymbolTrieNode _symbol_trie;

        std::unordered_map<std::string, Operator *> _operator_symbol_map;
        std::array<Operator *, static_cast<size_t>(Token::Type::t_unknown)> _predefined_operator_map;
    };
};

#endif
