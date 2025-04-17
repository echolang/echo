#ifndef ASTOPERATORSEMANTICS_H
#define ASTOPERATORSEMANTICS_H

#pragma once

#include "AST/ASTOps.h"
#include "AST/ASTValueType.h"

#include <string>

namespace AST
{
    class ExprNode;

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

    // a symbol-safe spelling of the above, for AST::mangle_function_name. every character outside
    // [A-Za-z0-9_] becomes `x` followed by two hex digits, so `operator +` mangles to `operatorx20x2b`
    //
    // the decorated name is what makes an operator's symbol unique, so it has to reach the mangled
    // name - but it holds a space and whatever the symbol is spelled out of, and an llvm symbol
    // carrying those is at best unportable and at worst rejected by the assembler
    std::string mangle_operator_name(const std::string &decorated_name);

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
    // ExprCodegen::gen_unary_expr lowers negation over a number, and nothing else. unary `+` never
    // reaches here at all - the parser folds it away, since it carries no semantics
    bool unary_has_builtin_meaning(const Operator *op, const OperandFacts &operand);
};

#endif
