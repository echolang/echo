#ifndef ASTOPERATORSEMANTICS_H
#define ASTOPERATORSEMANTICS_H

#pragma once

#include "AST/ASTOps.h"
#include "AST/ASTValueType.h"

#include <optional>
#include <string>
#include <vector>

namespace AST
{
    class Collector;
    class ExprNode;
    class FunctionCallExprNode;
    class FunctionDeclNode;
    class Module;

    // the name an `operator` declaration is registered under in the root namespace's overload set,
    // and the name a use site looks up: "operator +", "operator prefix !!", "operator suffix mm"
    //
    // **deliberately unspellable.** the space is what does it - no identifier may contain one - so an
    // operator can never collide with a function somebody wrote, and the whole of FunctionRegistry
    // works untouched instead of needing a second store keyed some other way. the same trick
    // Namespace::name() plays with `outer$3`
    //
    // the fixity is *in* the name because a prefix and a suffix operator on one symbol take one
    // parameter of the same type, so they are otherwise the same signature and would clash as a
    // DuplicateFunctionSignature. infix against unary already separates on arity, which
    // AST::match_function compares before it looks at any type
    std::string operator_function_name(const std::string &spelling, OpFixity fixity);

    // **the decorated name every index operator registers under**, computed once from the same two
    // inputs a caller would have written out. one asker, AST::OperatorRewriter::resolve_index, which needs
    // it to find the overload set
    const std::string &index_operator_name();

    // **the decorated name every index-*write* operator registers under**, `operator []=`.
    //
    // a **separate overload set** from index_operator_name's, and not an arity of it: a container may
    // declare both a borrowing bracket and a writing one, and `operator (M&)[K, K] : V&` beside
    // `operator (M&)[K] = (V) : void` are both three operands - so arity cannot tell the two *contracts*
    // apart and the name has to. which set a bracket asks is decided by the **position** it sits in, with
    // no fit rule in between
    const std::string &index_write_operator_name();

    // **which type is this operator declared over?** an operator is a member of neither operand's type -
    // it registers in the root namespace and there is no `_methods` list to ask - so the first operand
    // *is* the receiver, and its declared type read as a value is the whole of the answer:
    // `map<K, V>& $m` answers `map<K, V>`.
    //
    // unknown for a declaration with no operands, which every fixity's arity rule already refuses - so a
    // caller gets an answer that matches nothing rather than a special case to remember
    ValueType operator_receiver_type(const FunctionDeclNode &decl);

    // does `receiver` declare `$c[...] = v` as a call, for this arity. asked of the declarations, never
    // of the fit, or what a program means would rest on a ranking. array append is arity 0
    const FunctionDeclNode *find_index_write(Collector &collector, const ValueType &receiver, size_t index_count);

    // the borrowing bracket of the same arity: `operator []` with `index_count` indices.
    // array append is this with zero indices, returning `T&`
    const FunctionDeclNode *find_index(Collector &collector, const ValueType &receiver, size_t index_count);

    bool declares_index_write(Collector &collector, const ValueType &receiver, size_t index_count);

    // a symbol-safe spelling of the above, for AST::mangle_function_name. every character outside
    // [A-Za-z0-9_] becomes `x` followed by two hex digits, so `operator +` mangles to `operatorx20x2b`
    //
    // the decorated name is what makes an operator's symbol unique, so it has to reach the mangled
    // name - but it holds a space and whatever the symbol is spelled out of, and an llvm symbol
    // carrying those is at best unportable and at worst rejected by the assembler
    std::string mangle_operator_name(const std::string &decorated_name);

    // **the call an operator use site is.** an operator is a function, so a use site has nothing of
    // its own: a virtual name token holding the decorated name, positioned at the symbol so a
    // diagnostic points where a reader wrote it, and the root namespace as the lookup point - which
    // is where every `operator` declaration registers, so there is no outward walk to do and nothing
    // an inner namespace could hide (see Parser::parse_operatordecl, the other half of that decision)
    //
    // one owner, because there are two moments that build one: Parser::build_operator_call, where the
    // operand types are already known, and AST::OperatorRewriter, where they only became known this
    // round. the fixity is the caller's to name, once, in the gate that decided to come here
    //
    // the call is left **unresolved**. the parser drives its own settlement right after, the rewriter
    // leaves it to the fixpoint's settle_calls - and either way an unresolved operator call is a
    // legitimate intermediate state, because the declaration it names may still be instantiated
    FunctionCallExprNode &build_operator_call_node(
        Module &module,
        Collector &collector,
        const std::string &spelling,
        OpFixity fixity,
        const TokenReference &at,
        std::vector<ExprNode *> operands
    );

    // one operand as the rules below see it: the type a **value-position** read of it yields, plus
    // whether the user wrote `null` there - which has no type of its own, so it cannot be read back
    // off one
    struct OperandFacts
    {
        ValueType type = ValueType::make_unknown();
        bool is_null = false;
    };

    // the operand facts **at parse time**, where a place read of a borrow is still a pointer: `$this`
    // is a `Point&` and the deref that makes it a `Point` is inserted later by AST::PointerAdjuster.
    // so the value-position type is value_result_type's answer, not result_type's
    OperandFacts parse_time_operand(const ExprNode *expr);

    // the same, for a caller that already holds the operand's `result_type()` because the built-in
    // arms below the gate want the raw type too - Parser::parse_binary_expr is the one such site
    OperandFacts parse_time_operand(const ExprNode *expr, const ValueType &result_type);

    // the operand facts **after AST::PointerAdjuster**, where every deref is already a node in the
    // tree - so result_type() is the truth and asking value_result_type again would peel twice
    //
    // two named constructors rather than one function with a flag, so the two moments cannot be
    // confused at a call site: getting this backwards is silent, and what it silently does is let
    // pointer arithmetic be emitted for a struct
    OperandFacts adjusted_operand(const ExprNode *expr);

    // **does codegen lower this operator directly for these operands?**
    //
    // false is what makes a use site look for a declared `operator`, and it is also what the type
    // checker reports when none was found - one predicate, two readers, so "there is no built-in
    // meaning here" and "this is an error" can never come to different answers
    //
    // a custom symbol never has one: the language spells no meaning for `avg`. a built-in symbol has
    // one for every primitive and pointer combination ExprCodegen::gen_binary_expr enumerates, and
    // two complex cases - `==`/`!=` over two class handles, and `==`/`!=` against a written `null`
    // (presence, or a refusal). keeping the null case a BinaryExprNode is what keeps the rest of
    // the body concrete: an operator call with an untyped `null` left OwnershipPass unwalked
    bool binary_has_builtin_meaning(const Operator *op, const OperandFacts &lhs, const OperandFacts &rhs);

    // the same question for a unary operator, where the built-in surface is far smaller:
    // ExprCodegen::gen_unary_expr lowers negation over a number, `~` over an integer, and `!` over
    // a bool or anything that may be absent, and nothing else. unary `+` never reaches here at all
    // - the parser folds it away, since it carries no semantics
    bool unary_has_builtin_meaning(const Operator *op, const OperandFacts &operand);

    // **does this operator skip its right operand?** `&&` and `||`. LastRead and
    // OwnershipPass both ask it, so a skipped path cannot be a handover in one
    // walk and unconditional in the other. codegen and const_fold follow
    bool binary_skips_rhs(Token::Type op);

    // **does this symbol lower over one lane of this type?** the one matrix.
    //
    // `binary_operand_refusal` is the sentence. `SimdCodegen` throws if a pair this
    // answered no for still arrives. a vector use site stays builtin when this is
    // false — that is what keeps a declared operator from claiming it — except
    // `&&` / `||`, which `binary_has_builtin_meaning` answers on its own because the
    // short-circuit lowering is type-blind. unary `-` `!` `~` ask the same matrix
    bool simd_lane_op_lowers(Token::Type op, const ValueType &lane);

    // why these operands cannot answer this operator. asked after a refuse, not to decide whether
    // to. `$p == null` is binary_has_builtin_meaning instead
    std::optional<std::string> binary_operand_refusal(
        const Operator *op, const OperandFacts &lhs, const OperandFacts &rhs);

    // **and what to say when there is nothing operand-specific to say.** the wording a built-in symbol
    // has always had, spelled here so the two paths that reach it - the built-in one and the call one -
    // cannot drift. deliberately *not* for a custom symbol: `avg` and the bracket mean whatever their
    // declarations mean, so naming the candidates is the right answer for them
    std::string binary_unsupported_operands(
        const Operator *op, const OperandFacts &lhs, const OperandFacts &rhs);

    // integer meeting a float becomes the float; otherwise the wider wins. nullopt when they already
    // agree or one is not a number. parser and OperatorRewriter both ask
    std::optional<ValueType> common_numeric_type(const ValueType &lhs, const ValueType &rhs);

    // false for `<<` / `>>`: the rhs is a count, not a second value. reconciling anyway lets the
    // count's type flip an arithmetic shift to logical
    bool binary_reconciles_operands(const Operator *op);

    // the type a binary numeric operation is performed at. signedness cannot be taken from one side:
    // `int64 < uint32` is a signed comparison. a shift is performed at the left operand's type
    ValueType binary_operation_type(const Operator *op, const ValueType &lhs, const ValueType &rhs);

    // **the value a builtin operator produces for these peeled operands.** one owner, asked
    // by UnaryExprNode / BinaryExprNode and by structural conformance, so a vector comparison
    // cannot answer bool in one place and the operand vector in another.
    //
    // `!` is a mask over a simd and bool otherwise. a comparison is `simd<bool, N>` over a
    // vector and bool otherwise. arithmetic is binary_operation_type
    ValueType builtin_unary_result(Token::Type op, const ValueType &operand);
    ValueType builtin_binary_result(const Operator *op, const ValueType &lhs, const ValueType &rhs);

    // a shift count at or above the width is poison. const_fold and visitBinaryExpr both ask
    std::optional<std::string> shift_count_refusal(const ValueType &shifted, uint64_t count);
};

#endif
