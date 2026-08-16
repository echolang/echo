#ifndef ASTCFUNCTION_H
#define ASTCFUNCTION_H

#pragma once

#include "AST/ASTValueType.h"

#include <optional>
#include <string>
#include <vector>

namespace AST
{
    class CoreTypes;
    class ExprNode;
    class FunctionDeclNode;
    class FunctionRefExprNode;
    class FunctionRegistry;

    // **why may this signature not be a C function pointer?** nullopt when it may.
    //
    // asked once, by AST::TypeChecker, of every `extern function<R(P...)>` that appears on a
    // declaration - rather than at each of the several places a type is bound. one sweep is
    // what keeps a parameter, a return, a local and a property refused by the same sentence.
    //
    // primitives, `ptr<T>`, another C function pointer, and `void` as a return are allowed.
    // everything else is a shape echoc and clang would lower differently, or two words C has
    // no declaration for
    std::optional<std::string> c_function_signature_refusal(
        const CallableSignature &signature,
        const CoreTypes &core
    );

    // **why may this type not appear on a declaration, given it may hide an `extern function<...>`?**
    // nullopt when every C function pointer it contains is legal.
    //
    // asked of the type as a whole, including layout properties and statics, with a seen-set so a
    // recursive `S { S& $next }` does not loop. a walk that descended through pointers, weaks,
    // signatures and instantiation *arguments* and then stopped would miss a property of
    // `Handler<Point>` whose type was `extern function<Point(Point)>`
    std::optional<std::string> c_function_type_refusal(
        const ValueType &type,
        const CoreTypes &core
    );

    // **are these one C type?** Echo's `const` on a by-value parameter is a local write-protect
    // and is not part of C's convention, so `extern function<int32(const int32)>` and
    // `extern function<int32(int32)>` convert. the one comparison bind_function_ref_to and
    // is_implicitly_convertible both ask
    bool c_function_signatures_match(const CallableSignature &a, const CallableSignature &b);

    // **why may this declaration's address not be taken?** nullopt when it may.
    //
    // reads implicit_arg_count(), is_generic() and function_emission_kind rather than
    // re-deriving any. asked once, by AST::TypeChecker of a FunctionRefExprNode - the parser
    // mints the node and binds a unique candidate, it does not report this
    std::optional<std::string> c_function_ref_refusal(const FunctionDeclNode &decl);

    // the overload set `&name` denotes, re-derived from the node rather than stored - a stored
    // set goes stale the moment the tree is cloned for an instantiation, the same rule
    // CallResolver::candidates_for follows
    std::vector<FunctionDeclNode *> function_ref_candidates(
        const FunctionRefExprNode &node,
        FunctionRegistry &functions
    );

    // **gives an undecided `&name` the destination's signature.** the exact shape of
    // AST::bind_null_to beside it. a no-op on anything that is not an undecided function
    // ref, and on a destination that is not a C function pointer. the only writer of
    // `decl` that consults a destination - the parser's unique-candidate bind is the
    // other writer, and it does not look at types
    //
    // CallResolver is the only asker that can type a direct call's argument. the parser
    // asks where the destination is already known
    bool bind_function_ref_to(ExprNode *expr, const ValueType &destination, FunctionRegistry &functions);

    // the function ref this expression is, under implicit casts, or null
    FunctionRefExprNode *function_ref_of(ExprNode *expr);
    const FunctionRefExprNode *function_ref_of(const ExprNode *expr);
};

#endif
