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

    // **does `receiver` declare an element-*write* contract for `index_count` indices?** the one question
    // behind whether `$c[...] = v` is a single call that owns the whole write, or an ordinary write
    // through the place the borrowing bracket hands back.
    //
    // **asked of the declarations and never of the fit**, and that is the decision rather than an
    // implementation detail. Answered yes, an argument that does not fit is an ordinary NoMatchingOverload
    // against that one operator - which says more than a fallback would. Decided by *fit* instead, a
    // container declaring both brackets would have `$m[$absent] = $v` insert or assert depending on which
    // candidate AST::argument_fit happened to score higher, so what a program *means* would rest on a
    // ranking.
    //
    // **the arity is part of the question.** one set holds the append write `$c[] = v` (receiver plus
    // value) and the element write (receiver, indices, value), exactly as `operator []` holds the append
    // and the element forms - so a type may declare one and not the other, and `$a[3] = v` beside a
    // declared `$a[] = v` stays a write through a place. it is also what keeps the appends
    // AST::OperatorRewriter's literal expansion synthesizes out of this question entirely.
    //
    // the `template_or_self` redirect on both sides is what lets a `map<int32, string>` find the
    // `map<K, V>` template's declaration - AST::find_member_functions' idiom, which cannot be reused
    // directly here because it walks an owner's methods and an operator is never one
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
        std::vector<ExprNode *> operands);

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
    // exactly one complex case - `==`/`!=` over two class handles, which is how two references are
    // told apart and how a null one is detected
    bool binary_has_builtin_meaning(const Operator *op, const OperandFacts &lhs, const OperandFacts &rhs);

    // the same question for a unary operator, where the built-in surface is far smaller:
    // ExprCodegen::gen_unary_expr lowers negation over a number and `!` over a bool or anything that
    // may be absent, and nothing else. unary `+` never reaches here at all - the parser folds it away,
    // since it carries no semantics
    bool unary_has_builtin_meaning(const Operator *op, const OperandFacts &operand);

    // **why these two operands cannot answer this operator - said about the operands, not about a
    // candidate's parameters.** nullopt when there is nothing operand-level to say.
    //
    // one owner because there are three moments that refuse an operator and each used to phrase it its
    // own way. `TypeChecker::visitBinaryExpr` holds the built-in path; `AST::CallResolver` holds an
    // operator *call* nothing matched; and the type checker's argument coercion holds the case where the
    // matcher took a lone candidate without consulting types. Which of the three a use site reaches
    // depends on whether *anybody, anywhere in the program* declared an infix form of the symbol - the
    // parser's gate is `has_fixity`, so one `operator ==` in the standard library moved every
    // non-built-in `==` in every program from the first to the other two, and the advice that made the
    // message actionable went with it
    //
    // **a fallback rather than a pre-gate**, and that is load-bearing: `operator (P $a) == (P? $b)` is a
    // declaration a user may write, and it makes `$p == null` resolve. So this is only ever asked at a
    // site that has already decided to refuse - never to decide *whether* to
    std::optional<std::string> binary_operand_refusal(
        const Operator *op, const OperandFacts &lhs, const OperandFacts &rhs);

    // **and what to say when there is nothing operand-specific to say.** the wording a built-in symbol
    // has always had, spelled here so the two paths that reach it - the built-in one and the call one -
    // cannot drift. deliberately *not* for a custom symbol: `avg` and the bracket mean whatever their
    // declarations mean, so naming the candidates is the right answer for them
    std::string binary_unsupported_operands(
        const Operator *op, const OperandFacts &lhs, const OperandFacts &rhs);

    // **the type two mismatched numeric operands reconcile to.** an integer meeting a float becomes the
    // float, whatever the widths; otherwise the wider wins. nullopt when there is nothing to reconcile -
    // one of them is not a number, or they already agree - which is every operand pair but a few.
    //
    // one owner, for build_operator_call_node's reason: there are two *moments* that ask, and only one
    // rule. Parser::parse_binary_expr asks with the operand types it knows at parse time and inserts the
    // cast through try_implicit_cast, which may retype a literal outright instead. AST::OperatorRewriter
    // asks again for the operands a later pass typed - `foreach ($a as $i => $x) { if ($i == 0) ... }`,
    // where `$i` has no type until the loop lowers and the literal has long since defaulted to int32 -
    // and has only a TypeCastNode to wrap the losing side in.
    //
    // the insertion is each caller's, the decision is not: two answers here means one program means two
    // things depending on which pass got to type an operand first, and the way that surfaces is codegen
    // asserting on "Both operands to ICmp instruction are not of the same type"
    std::optional<ValueType> common_numeric_type(const ValueType &lhs, const ValueType &rhs);

    // **the type a binary numeric operation is performed at** - what the two operands were reconciled
    // to. the rule above's, read the other way round: a nullopt there means there was nothing to
    // reconcile, and for this question that is the operands already agreeing, so the lhs is the answer.
    //
    // what needs it is signedness, which cannot be taken from one side alone: `int64 < uint32`
    // reconciles to int64 and is therefore a *signed* comparison, so "either operand is unsigned" is a
    // second answer that differs from this one in exactly that case. ExprCodegen::gen_binary_expr reads
    // it for `/ % ** < > <= >=`, and AST::const_fold folds the same operators at the same type - two
    // answers there is a program whose `const if` takes one arm and whose `if` takes the other.
    //
    // AST::ConstFold still calls common_numeric_type itself, because it has to tell "no common type"
    // from "already agree" to report the first. one rule, and only one of its callers needs the failure
    ValueType binary_operation_type(const ValueType &lhs, const ValueType &rhs);
};

#endif
