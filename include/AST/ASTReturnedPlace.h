#ifndef ASTRETURNEDPLACE_H
#define ASTRETURNEDPLACE_H

#pragma once

#include "AST/ASTValueType.h"

#include <optional>
#include <string>
#include <vector>

namespace AST
{
    class ExprNode;
    class FunctionDeclNode;

    // **what place a T& method names**, walked from the callee's completing returns.
    // three answers, no tail. OwnershipPass is the only consumer: a T& into a dying compiler
    // temporary moves when this aliases the temp and the remainder needs no destructor
    enum class BorrowResultKind
    {
        // anything that is not a projection of `$this`: `at()`, a mix, no body, an interface
        // requirement, a non-pointer return
        t_none,

        // every completing `return` is `return $this`
        t_whole,

        // every completing `return` names the same member path of `$this`
        t_field,
    };

    struct BorrowResult
    {
        BorrowResultKind kind = BorrowResultKind::t_none;

        // property names from `$this` outward. empty on t_whole; one or more on t_field
        std::vector<std::string> path;
    };

    // a T& call chain composed to its root. `Probe()->add(1)->add(2)` is the constructor
    // call with an empty path; `make()->unwrap()` is `make()` with `__c0_value`
    struct BorrowChain
    {
        ExprNode *root = nullptr;
        std::vector<std::string> path;
    };

    // walks `fn`'s body (the instance a call landed on). a body that is not yet concrete
    // answers t_none for this round. instantiations are asked as themselves: the clone
    // carries the match the template has not resolved
    BorrowResult borrow_result_of(FunctionDeclNode *fn);

    // peels Deref, implicit casts, and T& calls `borrow_result_of` answers, down to the
    // value the chain names. none when a link does not name its receiver
    std::optional<BorrowChain> borrow_chain_of(ExprNode *expr);

    // moving `path` out of `holder` and skipping the container destructor is sound only
    // when the rest needs no destructor. empty path is always silent. a class, an
    // interface, and a user destructor fail closed. an enum overlay counts only the
    // live case's other fields
    bool remainder_needs_destruction(const ValueType &holder, const std::vector<std::string> &path);
};

#endif
