#ifndef ASTCONTROLFLOW_H
#define ASTCONTROLFLOW_H

#pragma once

namespace AST
{
    class ScopeNode;
    class ExprNode;

    // **how far does control go when it leaves this scope?** ordered, weakest first, so an `if` is the
    // weaker of its two arms and every caller's question is a comparison rather than a flag.
    //
    // the ordering is the whole content of the type: `break` leaves a scope, `return` leaves the function,
    // and a caller that wants one must not accept the other
    enum class ExitKind
    {
        // control falls out of the bottom of the scope
        t_none,

        // `break` / `continue` - the point past the closing brace is unreachable, but the enclosing
        // function carries on. the frames outside the loop are still live
        t_scope,

        // `return` / `die` - the enclosing function is done, and every frame with it
        t_function,
    };

    // **the kind of the first statement that leaves.** *first*, not "any": everything written after a
    // statement that left is unreachable, so a `return` written behind a `break` is dead code and not an
    // answer. asking "does any statement return" would find that dead `return` and report t_function for a
    // scope control actually leaves by breaking.
    //
    // that a stray statement at the tail must not change the answer is not academic - it is why this is a
    // walk at all. testing only the last child, a `return` followed by so much as one dead declaration
    // answered "falls through", and the ownership pass appended a second copy of the drop set it had
    // already collected onto that return's unwind
    //
    // deliberately structural rather than a reachability analysis - the *shape* of each statement, and for
    // an `if` the shape of its arms, recursively. codegen's own rule is the same shape:
    // StmtCodegen::gen_scope stops at the first terminated block. **it must only ever answer above t_none
    // when it is certain**, since a false positive is a leak in one caller and a broken binding in another
    ExitKind scope_exit_kind(const ScopeNode &scope);

    // **does evaluating this expression never come back?** the expression half of the question above, and
    // the reason it is worth naming separately is that a value can be written where a statement was
    // expected and both readings have to agree: `die('...')` is the same call whether it stands alone or
    // sits in a `match` arm, and only one of the two used to notice.
    //
    // AST::builtin_never_returns is what actually decides, through AST::BuiltinKind rather than by name,
    // so nothing here can drift from what stops the program. this is the walk down to the call: an
    // expression never returns iff it *is* one of those calls, which is deliberately narrower than "some
    // subexpression of it does not return". an argument that dies takes the whole call with it, but the
    // shapes that would reach - `f(die())` - are ones AST::TypeChecker refuses on the argument's `void`
    // type first, so widening this would be answering for programs that do not exist
    //
    // two readers, and they are the two halves of one rule: statement_exit_kind, so a bare `die('...')`
    // ends its scope, and AST::MatchResolution, so an arm that dies contributes no type to the arms'
    // unification. without the second, the only thing a `match` arm could do with a case whose payload is
    // the wrong type was produce a value it has not got - which is what stopped an enum from answering
    // `contract::unwrappable<V>::unwrap() : V&`
    bool expression_never_returns(const ExprNode &expr);

    // **can control fall out of the bottom of this scope?** the scope-boundary question, and the one two of
    // the three callers want:
    //
    // - `guard`'s else block, which has to leave. a guard binds a name that is only meaningful on the path
    //   where the value was there, so an else arm that ran on and rejoined would leave that name bound to
    //   nothing. refusing at the declaration is what makes the binding's promise true by construction
    //   rather than by the author remembering. a `break` satisfies it exactly as a `return` does - the
    //   binding is unreachable either way
    //
    // - AST::OwnershipPass, deciding whether a scope owes its locals a teardown *after* its last statement.
    //   when control never gets there, the drops the exit already collected onto its own unwind list are
    //   the whole story and a second set is dead tree - one that is still type-checked, and that mints a
    //   generic call site for a drop of a `Box<int32>` local
    inline bool scope_always_exits(const ScopeNode &scope)
    {
        return scope_exit_kind(scope) != ExitKind::t_none;
    }

    // **does every path leave the enclosing function?** the constructor parser's question, deciding whether
    // to append the implicit `return $this`. a constructor whose body already returned has nowhere to put
    // one, and the return appended behind it is not merely dead: a body whose arms all `return $this` moved
    // it out on every path, so the extra read was refused with "'$this' has been moved out of" and the
    // constructor did not compile at all
    //
    // **deliberately not the predicate above.** a loop body's exit is not the function's, so a `break`
    // answers true there and false here. two named predicates rather than one with a flag, for
    // AST::parse_time_operand's reason - a call site that gets it backwards fails silently, and here it
    // fails as a constructor that returns undef
    inline bool scope_always_leaves_function(const ScopeNode &scope)
    {
        return scope_exit_kind(scope) == ExitKind::t_function;
    }
};

#endif
